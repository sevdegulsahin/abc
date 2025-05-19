#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <json-c/json.h>
#include <stdbool.h> // bool için
#include "list.h"
#include "drone.h"
#include "survivor.h"
#include "view.h" // view.h'nin gerekli olup olmadığını kontrol edin, burada doğrudan kullanılmıyor gibi.
#include <errno.h>
#define PORT 8080
#define VIEW_PORT 8081
#define MAX_DRONES 50
#define MAX_VIEWS 10
#define RECV_BUFFER_SIZE 4096 // Gelen veri için tampon boyutu

// Fonksiyon prototipleri (eğer view.h'de değilse)
// void free_survivor(void *survivor_data); // Zaten survivor.h'de olmalı
// Drone *create_drone(int id, int x, int y); // Zaten drone.h'de olmalı
// void free_drone(void *drone_data); // Zaten drone.h'de olmalı

int compare_survivor(void *a, void *b)
{
    Survivor *s1 = (Survivor *)a;
    Survivor *s2 = (Survivor *)b;
    return s1->id - s2->id; // ID'ye göre karşılaştırma
}

int compare_drone(void *a, void *b)
{
    Drone *d1 = (Drone *)a;
    Drone *d2 = (Drone *)b;
    return d1->id - d2->id; // ID'ye göre karşılaştırma
}

List *drone_list;
List *survivor_list;
List *view_sockets;

void send_json(int sock, json_object *jobj)
{
    if (sock <= 0)
        return; // Geçersiz soket
    const char *str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    if (!str)
    {
        fprintf(stderr, "Error: json_object_to_json_string failed.\n");
        return;
    }
    send(sock, str, strlen(str), 0);
    send(sock, "\n", 1, 0); // Mesaj sonu için newline
}

void *handle_drone(void *arg)
{
    int sock = *(int *)arg;
    free(arg); // arg'ı hemen serbest bırak

    char recv_buffer[RECV_BUFFER_SIZE];
    char process_buffer[RECV_BUFFER_SIZE * 2] = {0}; // Birikmiş mesajları işlemek için
    int process_buffer_len = 0;
    Drone *drone = NULL; // Bu thread tarafından yönetilen drone

    printf("Drone handler thread started for socket %d\n", sock);

    while (1)
    {
        int len = recv(sock, recv_buffer, sizeof(recv_buffer) - 1, 0);
        if (len <= 0)
        {
            if (len == 0)
            {
                printf("Drone disconnected (socket: %d)\n", sock);
            }
            else
            {
                perror("recv failed for drone socket");
            }
            break; // Bağlantı kapandı veya hata oluştu
        }
        recv_buffer[len] = '\0';

        // Gelen veriyi process_buffer'a ekle
        if (process_buffer_len + len < sizeof(process_buffer))
        {
            strcat(process_buffer, recv_buffer);
            process_buffer_len += len;
        }
        else
        {
            fprintf(stderr, "Process buffer overflow for drone socket %d. Discarding data.\n", sock);
            process_buffer[0] = '\0';
            process_buffer_len = 0;
            continue;
        }

        char *msg_start = process_buffer;
        char *msg_end;

        // Bufferdaki tüm tam JSON mesajlarını işle (\n ile ayrılmış)
        while ((msg_end = strchr(msg_start, '\n')) != NULL)
        {
            *msg_end = '\0'; // Mesajı null-terminate et

            json_object *jobj = json_tokener_parse(msg_start);
            if (!jobj)
            {
                // fprintf(stderr, "Failed to parse JSON from drone socket %d: %s\n", sock, msg_start);
                msg_start = msg_end + 1; // Bir sonraki potansiyel mesaja geç
                continue;
            }

            const char *type_str = json_object_get_string(json_object_object_get(jobj, "type"));
            if (!type_str)
            {
                fprintf(stderr, "Received JSON from drone socket %d without 'type' field.\n", sock);
                json_object_put(jobj);
                msg_start = msg_end + 1;
                continue;
            }

            if (strcmp(type_str, "HANDSHAKE") == 0)
            {
                const char *drone_id_json = json_object_get_string(json_object_object_get(jobj, "drone_id"));
                if (!drone_id_json)
                {
                    fprintf(stderr, "HANDSHAKE message from drone socket %d missing 'drone_id'.\n", sock);
                    json_object_put(jobj);
                    msg_start = msg_end + 1;
                    continue;
                }
                char id_str_buf[20]; // ID string için güvenli buffer
                strncpy(id_str_buf, drone_id_json, sizeof(id_str_buf) - 1);
                id_str_buf[sizeof(id_str_buf) - 1] = '\0';

                char *id_ptr = id_str_buf;
                if (id_ptr[0] == 'D')
                    id_ptr++;
                int id = atoi(id_ptr);

                // Aynı ID ile zaten bağlı bir drone var mı kontrol et (opsiyonel ama iyi bir pratik)
                bool already_exists = false;
                pthread_mutex_lock(&drone_list->lock);
                Node *temp_node = drone_list->head;
                while (temp_node)
                {
                    Drone *existing_drone = (Drone *)temp_node->data;
                    if (existing_drone->id == id)
                    {
                        already_exists = true;
                        break;
                    }
                    temp_node = temp_node->next;
                }
                pthread_mutex_unlock(&drone_list->lock);

                if (already_exists)
                {
                    printf("Drone with ID %d (socket %d) tried to connect again. Closing new connection.\n", id, sock);
                    json_object_put(jobj);
                    close(sock); // Bu thread'i ve soketi sonlandır
                    // Eğer `drone` pointer'ı bu aşamada atanmışsa, temizlik yapılmalı, ancak burada atanmıyor.
                    return NULL; // Thread'i sonlandır
                }

                drone = create_drone(id, -1, -1); // Rastgele başlangıç konumu
                if (!drone)
                {
                    fprintf(stderr, "Failed to create drone object for ID %d (socket %d).\n", id, sock);
                    json_object_put(jobj);
                    // close(sock); // Zaten döngü sonunda kapanacak
                    // return NULL; // Hata durumunda thread'i sonlandır
                    msg_start = msg_end + 1;
                    continue; // Veya thread'i sonlandır
                }
                drone->sock = sock;
                add_list(drone_list, drone); // Listeye ekle
                printf("Drone D%d (socket %d) connected and added to list.\n", id, sock);

                json_object *ack = json_object_new_object();
                json_object_object_add(ack, "type", json_object_new_string("HANDSHAKE_ACK"));
                // ... (diğer ACK alanları)
                send_json(sock, ack);
                json_object_put(ack);
            }
            else if (drone && strcmp(type_str, "STATUS_UPDATE") == 0)
            {
                pthread_mutex_lock(&drone->lock);
                json_object *loc_obj = json_object_object_get(jobj, "location");
                if (loc_obj)
                {
                    drone->coord.x = json_object_get_int(json_object_object_get(loc_obj, "x"));
                    drone->coord.y = json_object_get_int(json_object_object_get(loc_obj, "y"));
                }
                const char *status_str = json_object_get_string(json_object_object_get(jobj, "status"));
                if (status_str)
                {
                    drone->status = (strcmp(status_str, "idle") == 0) ? IDLE : ON_MISSION;
                }
                drone->battery = json_object_get_int(json_object_object_get(jobj, "battery"));
                // printf("Drone D%d status: (%d,%d), %s, Bat: %d\n", drone->id, drone->coord.x, drone->coord.y, drone->status == IDLE ? "IDLE" : "ON_MISSION", drone->battery);
                pthread_mutex_unlock(&drone->lock);
            }
            else if (drone && strcmp(type_str, "MISSION_COMPLETE") == 0)
            {
                Coordinate completed_mission_target;
                pthread_mutex_lock(&drone->lock);
                drone->status = IDLE;
                completed_mission_target = drone->target; // Görevin hedefi buydu
                printf("Drone D%d reported MISSION_COMPLETE for target (%d,%d). Current pos: (%d,%d)\n",
                       drone->id, completed_mission_target.x, completed_mission_target.y, drone->coord.x, drone->coord.y);
                pthread_mutex_unlock(&drone->lock);

                // 1. Kurtarılan survivor'ı bul ve listeden çıkar
                bool survivor_removed = false;
                pthread_mutex_lock(&survivor_list->lock);
                Node *current_s_node = survivor_list->head;
                Node *prev_s_node = NULL;
                while (current_s_node != NULL)
                {
                    Survivor *s = (Survivor *)current_s_node->data;
                    // Görevin hedefi ile survivor koordinatları eşleşmeli
                    if (s->coord.x == completed_mission_target.x && s->coord.y == completed_mission_target.y)
                    {
                        printf("Survivor S%d at (%d,%d) was targeted by drone D%d and is now rescued.\n",
                               s->id, s->coord.x, s->coord.y, drone->id);
                        if (prev_s_node == NULL)
                        { // Listenin başı
                            survivor_list->head = current_s_node->next;
                        }
                        else
                        {
                            prev_s_node->next = current_s_node->next;
                        }
                        Node *to_free = current_s_node;
                        current_s_node = current_s_node->next; // İteratörü ilerlet
                        free_survivor(to_free->data);
                        free(to_free);
                        survivor_list->size--;
                        survivor_removed = true;
                        break; // Bu survivor için işlem tamam
                    }
                    else
                    {
                        prev_s_node = current_s_node;
                        current_s_node = current_s_node->next;
                    }
                }
                if (!survivor_removed)
                {
                    printf("Warning: Drone D%d completed mission at target (%d,%d), but no matching survivor found in list or it was already removed.\n",
                           drone->id, completed_mission_target.x, completed_mission_target.y);
                }

                // 2. Drone IDLE durumda, yeni bir görev ata (eğer uygun survivor varsa)
                Survivor *next_survivor_to_assign = NULL;
                int min_dist = -1; // Başlangıçta -1, ilk uygun survivor mesafesi olacak

                // Drone'un mevcut pozisyonunu al (yeni görev için)
                Coordinate drone_current_pos_for_new_mission;
                pthread_mutex_lock(&drone->lock); // drone kilidini kısa süreliğine al
                drone_current_pos_for_new_mission = drone->coord;
                pthread_mutex_unlock(&drone->lock);

                current_s_node = survivor_list->head; // Kalan survivor listesini tekrar tara
                while (current_s_node)
                {
                    Survivor *s_candidate = (Survivor *)current_s_node->data;
                    if (!s_candidate->is_targeted)
                    { // Sadece hedeflenmemiş olanlar
                        int dist = abs(drone_current_pos_for_new_mission.x - s_candidate->coord.x) +
                                   abs(drone_current_pos_for_new_mission.y - s_candidate->coord.y);
                        if (next_survivor_to_assign == NULL || dist < min_dist)
                        {
                            min_dist = dist;
                            next_survivor_to_assign = s_candidate;
                        }
                    }
                    current_s_node = current_s_node->next;
                }

                if (next_survivor_to_assign)
                {
                    pthread_mutex_lock(&drone->lock);
                    if (drone->status == IDLE)
                    { // Drone hala boşta mı kontrol et (çoklu thread güvenliği)
                        drone->status = ON_MISSION;
                        drone->target = next_survivor_to_assign->coord;
                        next_survivor_to_assign->is_targeted = true;

                        json_object *mission_jobj = json_object_new_object();
                        json_object_object_add(mission_jobj, "type", json_object_new_string("ASSIGN_MISSION"));
                        // Benzersiz mission_id oluştur (opsiyonel)
                        char mission_id_str[30];
                        snprintf(mission_id_str, sizeof(mission_id_str), "M_D%dS%d_T%ld", drone->id, next_survivor_to_assign->id, time(NULL));
                        json_object_object_add(mission_jobj, "mission_id", json_object_new_string(mission_id_str));
                        json_object *target_loc_jobj = json_object_new_object();
                        json_object_object_add(target_loc_jobj, "x", json_object_new_int(next_survivor_to_assign->coord.x));
                        json_object_object_add(target_loc_jobj, "y", json_object_new_int(next_survivor_to_assign->coord.y));
                        json_object_object_add(mission_jobj, "target", target_loc_jobj);
                        send_json(drone->sock, mission_jobj);
                        json_object_put(mission_jobj);

                        printf("Drone D%d (after mission complete) assigned to new survivor S%d at (%d,%d).\n",
                               drone->id, next_survivor_to_assign->id, next_survivor_to_assign->coord.x, next_survivor_to_assign->coord.y);
                    }
                    pthread_mutex_unlock(&drone->lock);
                }
                else
                {
                    printf("Drone D%d is IDLE. No available non-targeted survivors for new mission.\n", drone->id);
                }
                pthread_mutex_unlock(&survivor_list->lock);
            }
            else if (drone && strcmp(type_str, "BATTERY_DEPLETED") == 0)
            {
                printf("Drone D%d battery depleted. Removing from list.\n", drone->id);
                // `drone` zaten bu thread'e ait, listeden çıkarılıp free edilecek.
                // Diğer thread'ler artık bu drone'a erişememeli.
                // `drone` pointer'ını NULL yapmak bu scope'ta yeterli, list'ten çıkarma aşağıda yapılacak.
                json_object_put(jobj); // jobj'yi serbest bırak
                // break; // Ana döngüyü kır, temizlik aşağıda yapılacak
                // Direkt thread'i sonlandırmak daha temiz olabilir bu durumda
                // Temizlik yap ve çık
                if (drone)
                {
                    pthread_mutex_lock(&drone_list->lock);
                    // compare_drone ID ile karşılaştırmalı, pointer ile değil, eğer ID'ye göre arıyorsak.
                    // Ancak remove_list genellikle data pointer'ı ile karşılaştırır.
                    // ID'ye göre özel bir remove fonksiyonu daha iyi olabilirdi veya remove_list'in ID'ye göre aramasını sağlamak.
                    // Şimdilik data pointer'ı ile remove_list'in çalıştığını varsayalım.
                    // Eğer remove_list(drone_list, drone, compare_drone_by_id) gibi bir fonksiyon olsaydı,
                    // ve compare_drone_by_id(void* list_data, void* id_to_find) şeklinde olsaydı daha iyi olurdu.
                    // Mevcut remove_list(list, data, compare_func) için compare_func (void* a, void* b)'yi alır.
                    // Bu yüzden listedeki elemanlarla doğrudan 'drone' pointer'ını karşılaştırır.
                    remove_list(drone_list, drone, compare_drone); // drone_list'ten kaldır
                    pthread_mutex_unlock(&drone_list->lock);

                    // free_drone içinde mutex_destroy var, kilidi tutarken çağırma.
                    // drone->sock'u kapatıp free_drone çağırmak en iyisi.
                    close(drone->sock); // Soketi kapat
                    drone->sock = -1;   // Soketin kapalı olduğunu işaretle
                    free_drone(drone);  // Drone'u serbest bırak
                    drone = NULL;       // Pointer'ı NULL yap
                }
                return NULL; // Thread'i sonlandır
            }
            else if (drone)
            {
                printf("Drone D%d sent unknown message type: %s\n", drone->id, type_str);
            }
            else
            {
                printf("Received message type '%s' from socket %d but no drone context established (handshake not done?).\n", type_str, sock);
            }

            json_object_put(jobj);   // İşlenen JSON nesnesini serbest bırak
            msg_start = msg_end + 1; // Bir sonraki mesajın başlangıcına git
        }

        // Kalan (tamamlanmamış) mesajı buffer'ın başına taşı
        if (*msg_start != '\0')
        {
            memmove(process_buffer, msg_start, strlen(msg_start) + 1);
            process_buffer_len = strlen(process_buffer);
        }
        else
        {
            process_buffer[0] = '\0';
            process_buffer_len = 0;
        }
    }

    // Döngüden çıkıldı (bağlantı kesildi veya hata)
    printf("Drone handler for socket %d is terminating.\n", sock);
    if (drone)
    {
        printf("Cleaning up for drone D%d (socket %d).\n", drone->id, sock);
        pthread_mutex_lock(&drone_list->lock);
        remove_list(drone_list, drone, compare_drone); // Drone'u aktif listeden çıkar
        pthread_mutex_unlock(&drone_list->lock);

        if (drone->sock > 0)
        { // Soket BATTERY_DEPLETED tarafından kapatılmadıysa
            close(drone->sock);
            drone->sock = -1; // Artık geçersiz
        }
        free_drone(drone); // Drone kaynaklarını serbest bırak
        drone = NULL;
    }
    else
    {
        // `drone` hiç atanmadıysa (örneğin handshake tamamlanmadıysa)
        // ama soket açıksa, kapat.
        if (sock > 0)
            close(sock);
    }

    return NULL;
}

void *controller(void *arg)
{
    while (1)
    {
        sleep(2); // Periyodik kontrol (örn: 2 saniyede bir)

        pthread_mutex_lock(&drone_list->lock);
        Node *d_node = drone_list->head;
        while (d_node)
        {
            Drone *d = (Drone *)d_node->data;
            pthread_mutex_lock(&d->lock); // Bireysel drone kilidi

            if (d->status == IDLE && d->sock > 0)
            {                                   // Sadece IDLE ve hala bağlı olan droneları değerlendir
                pthread_mutex_unlock(&d->lock); // Survivor ararken drone kilidini serbest bırak

                Survivor *closest_unassigned_survivor = NULL;
                int min_dist = -1;
                Coordinate drone_current_pos;

                pthread_mutex_lock(&d->lock); // Drone pozisyonunu almak için kilidi tekrar al
                drone_current_pos = d->coord;
                pthread_mutex_unlock(&d->lock); // Hemen bırak

                pthread_mutex_lock(&survivor_list->lock);
                Node *s_node = survivor_list->head;
                while (s_node)
                {
                    Survivor *s = (Survivor *)s_node->data;
                    if (!s->is_targeted)
                    { // Sadece henüz bir drone atanmamış survivor'ları değerlendir
                        int dist = abs(drone_current_pos.x - s->coord.x) +
                                   abs(drone_current_pos.y - s->coord.y);
                        if (closest_unassigned_survivor == NULL || dist < min_dist)
                        {
                            min_dist = dist;
                            closest_unassigned_survivor = s;
                        }
                    }
                    s_node = s_node->next;
                }

                if (closest_unassigned_survivor)
                {
                    // Drone'a görevi ata
                    pthread_mutex_lock(&d->lock);
                    // Son bir kontrol: Drone hala IDLE mı ve soket hala geçerli mi?
                    if (d->status == IDLE && d->sock > 0)
                    {
                        d->status = ON_MISSION;
                        d->target = closest_unassigned_survivor->coord;
                        closest_unassigned_survivor->is_targeted = true; // Survivor'ı hedeflenmiş olarak işaretle

                        json_object *mission_jobj = json_object_new_object();
                        json_object_object_add(mission_jobj, "type", json_object_new_string("ASSIGN_MISSION"));
                        char mission_id_str[30];
                        snprintf(mission_id_str, sizeof(mission_id_str), "M_Ctrl_D%dS%d_T%ld", d->id, closest_unassigned_survivor->id, time(NULL));
                        json_object_object_add(mission_jobj, "mission_id", json_object_new_string(mission_id_str));
                        json_object *target_loc_jobj = json_object_new_object();
                        json_object_object_add(target_loc_jobj, "x", json_object_new_int(closest_unassigned_survivor->coord.x));
                        json_object_object_add(target_loc_jobj, "y", json_object_new_int(closest_unassigned_survivor->coord.y));
                        json_object_object_add(mission_jobj, "target", target_loc_jobj);
                        send_json(d->sock, mission_jobj);
                        json_object_put(mission_jobj);

                        printf("Controller: Assigned drone D%d to survivor S%d at (%d,%d).\n",
                               d->id, closest_unassigned_survivor->id,
                               closest_unassigned_survivor->coord.x, closest_unassigned_survivor->coord.y);
                    }
                    else
                    {
                        // Drone durumu değişmiş veya bağlantı kopmuş olabilir.
                        if (closest_unassigned_survivor)
                            closest_unassigned_survivor->is_targeted = false; // Atama başarısız oldu, işareti geri al
                    }
                    pthread_mutex_unlock(&d->lock);
                }
                pthread_mutex_unlock(&survivor_list->lock);
            }
            else
            { // Drone ON_MISSION veya soket kapalı
                pthread_mutex_unlock(&d->lock);
            }
            d_node = d_node->next;
        }
        pthread_mutex_unlock(&drone_list->lock);
    }
    return NULL;
}

void *survivor_generator(void *arg)
{
    static int survivor_id_counter = 0; // Benzersiz ID için
    while (1)
    {
        sleep(10);                       // Yeni survivor üretme sıklığı (örneğin 10 saniyede bir)
        int x = rand() % 40;             // MAP_HEIGHT (0-39)
        int y = rand() % 60;             // MAP_WIDTH  (0-59)
        int priority = (rand() % 3) + 1; // 1, 2, veya 3

        Survivor *s = create_survivor(survivor_id_counter++, x, y, priority);
        if (s)
        {
            add_list(survivor_list, s);
            printf("Generated survivor S%d at (%d,%d), priority %d.\n", s->id, x, y, priority);
        }
    }
    return NULL;
}

int compare_view_socket(void *a, void *b)
{
    int sock_a = *(int *)a;
    int sock_b = *(int *)b;
    if (sock_a < sock_b)
        return -1;
    if (sock_a > sock_b)
        return 1;
    return 0;
}

void *handle_view_client(void *arg)
{
    int sock = *(int *)arg; // `arg` zaten int*, içindeki değeri al
    // free(arg) burada yapılmamalı eğer `main` döngüsündeki `view_fd_ptr` `add_list` ile listeye eklendiyse
    // ve `destroy_list` `free` ile onu serbest bırakacaksa. Eğer `add_list` pointer'ın kopyasını saklıyorsa
    // ve `arg` sadece bu thread'e geçici bir referanssa, o zaman `free(arg)` doğru olurdu.
    // Mevcut `add_list` verinin kendisini (pointer'ı) saklar.
    // `remove_list` çağrısında `free_data` callback'i yoksa, sadece node serbest kalır, data (int*) değil.
    // Bu yüzden `main`de `destroy_list(view_sockets, free)` kullanılması doğru.
    // Bu thread'den çıkarken `remove_list` `compare_view_socket` ile soketi bulup sadece node'u siler.
    // `int*` verisi `destroy_list` tarafından serbest bırakılır.

    printf("View client handler started for socket %d\n", sock);
    char buffer[1024];
    bool handshake_done = false;

    // İlk mesajın handshake olmasını bekle
    int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (len > 0)
    {
        buffer[len] = '\0';
        char *newline = strchr(buffer, '\n'); // Tek JSON mesajı varsayımı
        if (newline)
            *newline = '\0';

        json_object *jobj = json_tokener_parse(buffer);
        if (jobj)
        {
            const char *type = json_object_get_string(json_object_object_get(jobj, "type"));
            if (type && strcmp(type, "VIEW_HANDSHAKE") == 0)
            {
                // int *sock_ptr = malloc(sizeof(int)); // Zaten main'de malloc edildi
                // *sock_ptr = sock;
                // add_list(view_sockets, sock_ptr); // main'de accept sonrası eklenecek. Burada tekrar ekleme.
                // Bunun yerine, handshake başarılı ise bir flag set edilebilir.
                handshake_done = true;
                printf("View client (socket %d) handshake successful.\n", sock);

                // Handshake ACK (opsiyonel)
                json_object *ack = json_object_new_object();
                json_object_object_add(ack, "type", json_object_new_string("VIEW_HANDSHAKE_ACK"));
                send_json(sock, ack);
                json_object_put(ack);
            }
            else
            {
                printf("View client (socket %d) sent non-handshake message first. Closing.\n", sock);
            }
            json_object_put(jobj);
        }
        else
        {
            printf("View client (socket %d) sent invalid JSON for handshake. Closing.\n", sock);
        }
    }
    else
    {
        printf("View client (socket %d) disconnected before handshake or recv error.\n", sock);
    }

    if (!handshake_done)
    {
        // remove_list için bir karşılaştırma fonksiyonu lazım eğer `sock` değeri ile arama yapılacaksa.
        // Veya `main`de `view_sockets` listesine eklenen `int*` pointer'ı saklayıp onunla `remove_list` çağırmak.
        // Şu anki `remove_list(view_sockets, &sock, NULL)` `NULL` compare ile çalışmaz.
        // `compare_view_socket` fonksiyonu eklendi.
        // `add_list` `int*` sakladığı için, `remove_list` de `int*` adresi ile `compare_view_socket` kullanarak çalışmalı.
        // Ancak `sock` burada stack değişkeni, `view_sockets` listesindeki heap'ten alınmış `int*` ile eşleşmez.
        // Bu yüzden view_sockets'tan çıkarmak için listenin taranması veya `sock` değerini içeren `int*`'ın bulunması gerekir.
        // Şimdilik, `main`deki `destroy_list` temizliğe güvensin.
        // Daha iyisi: handle_view_client'a view_sockets listesindeki Node'un pointer'ını geçmek.
        // Ya da `view_broadcast`te gönderme hatası olursa listeden çıkarmak.
        close(sock);
        return NULL;
    }

    // Handshake başarılı olduktan sonra, istemci genelde sadece veri alır, göndermez.
    // Eğer istemciden başka komutlar bekleniyorsa buraya eklenebilir.
    // Şimdilik, bağlantının açık kalıp kalmadığını kontrol et.
    while (1)
    {
        // view client'ından mesaj beklemek yerine, sadece bağlantı koparsa diye kontrol et.
        // Veya heartbeat gibi bir şey beklenebilir.
        // Şimdilik basitçe, eğer view_broadcast hata verirse listeden çıkarılacak.
        // Bu thread'in asıl işi view_broadcast'in veri göndermesi.
        // Buradaki recv sadece bağlantının kesilip kesilmediğini anlamak için.
        len = recv(sock, buffer, sizeof(buffer) - 1, MSG_DONTWAIT); // Non-blocking recv
        if (len == 0)
        { // Bağlantı düzgün kapatıldı
            printf("View client (socket %d) disconnected.\n", sock);
            break;
        }
        if (len < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                // Veri yok, normal durum. Beklemeye devam et.
                sleep(1); // CPU'yu yormamak için bekle
                continue;
            }
            else
            { // Gerçek bir hata
                perror("recv error from view client");
                break;
            }
        }
        // Eğer bir veri geldiyse (beklenmedik), logla ve atla
        buffer[len] = '\0';
        printf("Unexpected data from view client (socket %d): %s\n", sock, buffer);
    }

    // Bağlantı kesildi veya hata. View_sockets listesinden çıkar.
    // `main`deki `accept` döngüsünde `view_fd_ptr` malloc ile oluşturulup `handle_view_client`'e `arg` olarak geçiliyor.
    // `add_list` de bu `view_fd_ptr`'ı saklıyor.
    // `remove_list` çağrısında `data` argümanı `view_fd_ptr` olmalı.
    // Ancak bu `arg` fonksiyon başında `free(arg)` ile serbest bırakılmıştı. Bu bir sorun.
    // `arg` serbest bırakılmamalı, onun yerine listenen çıkarılacak `int*` bu olmalı.
    // Şimdilik `view_broadcast` içinde `send` hatası olursa oradan çıkarılmasını bekleyeceğiz.
    // Ya da `handle_view_client`'e `int sock_val` yerine `int* sock_ptr_in_list` geçirilmeli.

    // Düzeltme: handle_view_client'ın başında `arg` free edilmemeli,
    // bunun yerine listenen çıkarılacak `int*` bu olmalı.
    // Bu durumda `int sock = *(int *)arg;` yerine `int* sock_ptr_in_list = (int*)arg;`
    // ve `int sock = *sock_ptr_in_list;` kullanılmalı.
    // Çıkarken `remove_list(view_sockets, sock_ptr_in_list, compare_exact_pointer);`
    // `compare_exact_pointer` da `(void*a, void*b) { return a == b ? 0 : 1; }` gibi olur.
    // Ya da `compare_view_socket` ile `*sock_ptr_in_list` değerini içeren node'u bulmak.
    // Şimdilik en basit yol, `view_broadcast` içinde `send` hatası durumunda çıkarmak.
    // Bu thread sadece `close(sock)` yapsın.
    printf("View client handler for socket %d terminating.\n", sock);
    close(sock); // Soketi kapat
    // Listeden çıkarma işlemini view_broadcast'e bırakalım (send hatası durumunda)
    // veya main destroy_list ile halletsin.
    // Eğer view_sockets listesinde *sock değeri* ile arama yapacaksak remove_list
    // ve compare_view_socket kullanılabilir, ama remove_list free_data'yı çağırmaz (NULL ise).
    // Bu yüzden `free(arg)` `destroy_list`'te yapılmalı.
    return NULL;
}

void *view_broadcast(void *arg)
{
    (void)arg; // arg kullanılmıyor
    while (1)
    {
        sleep(1); // Saniyede bir güncelleme

        json_object *state_jobj = json_object_new_object();
        json_object_object_add(state_jobj, "type", json_object_new_string("STATE_UPDATE"));

        // Droneları ekle
        json_object *drones_arr = json_object_new_array();
        pthread_mutex_lock(&drone_list->lock);
        Node *d_node = drone_list->head;
        while (d_node)
        {
            Drone *d = (Drone *)d_node->data;
            pthread_mutex_lock(&d->lock);
            if (d->sock > 0)
            { // Sadece aktif soketi olan dronelar
                json_object *d_obj = json_object_new_object();
                json_object_object_add(d_obj, "id", json_object_new_int(d->id));
                // ... (diğer drone bilgileri: location, status, target, battery)
                json_object *loc = json_object_new_object();
                json_object_object_add(loc, "x", json_object_new_int(d->coord.x));
                json_object_object_add(loc, "y", json_object_new_int(d->coord.y));
                json_object_object_add(d_obj, "location", loc);
                json_object_object_add(d_obj, "status", json_object_new_string(d->status == IDLE ? "idle" : "busy"));
                json_object *target = json_object_new_object();
                json_object_object_add(target, "x", json_object_new_int(d->target.x));
                json_object_object_add(target, "y", json_object_new_int(d->target.y));
                json_object_object_add(d_obj, "target", target);
                json_object_object_add(d_obj, "battery", json_object_new_int(d->battery));
                json_object_array_add(drones_arr, d_obj);
            }
            pthread_mutex_unlock(&d->lock);
            d_node = d_node->next;
        }
        pthread_mutex_unlock(&drone_list->lock);
        json_object_object_add(state_jobj, "drones", drones_arr);

        // Survivor'ları ekle
        json_object *survivors_arr = json_object_new_array();
        pthread_mutex_lock(&survivor_list->lock);
        Node *s_node = survivor_list->head;
        while (s_node)
        {
            Survivor *s = (Survivor *)s_node->data;
            json_object *s_obj = json_object_new_object();
            json_object_object_add(s_obj, "id", json_object_new_int(s->id));
            // ... (diğer survivor bilgileri: location, priority)
            json_object *loc = json_object_new_object();
            json_object_object_add(loc, "x", json_object_new_int(s->coord.x));
            json_object_object_add(loc, "y", json_object_new_int(s->coord.y));
            json_object_object_add(s_obj, "location", loc);
            json_object_object_add(s_obj, "priority", json_object_new_int(s->priority));
            // json_object_object_add(s_obj, "is_targeted", json_object_new_boolean(s->is_targeted)); // GUI için faydalı olabilir
            json_object_array_add(survivors_arr, s_obj);
            s_node = s_node->next;
        }
        pthread_mutex_unlock(&survivor_list->lock);
        json_object_object_add(state_jobj, "survivors", survivors_arr);

        // Tüm bağlı view client'larına gönder
        pthread_mutex_lock(&view_sockets->lock);
        Node *v_node = view_sockets->head;
        Node *v_prev = NULL;
        while (v_node)
        {
            int *sock_ptr = (int *)v_node->data;
            if (sock_ptr && *sock_ptr > 0)
            {
                const char *json_str = json_object_to_json_string_ext(state_jobj, JSON_C_TO_STRING_PLAIN);
                if (json_str)
                {
                    if (send(*sock_ptr, json_str, strlen(json_str), MSG_NOSIGNAL) < 0 ||
                        send(*sock_ptr, "\n", 1, MSG_NOSIGNAL) < 0)
                    {
                        perror("send to view client failed");
                        printf("View client (socket %d) disconnected, removing from broadcast list.\n", *sock_ptr);
                        close(*sock_ptr); // Soketi kapat
                        free(sock_ptr);   // Malloc ile alınan int* 'ı serbest bırak

                        Node *to_remove = v_node;
                        if (v_prev)
                        {
                            v_prev->next = v_node->next;
                            v_node = v_node->next;
                        }
                        else
                        { // Listenin başı
                            view_sockets->head = v_node->next;
                            v_node = view_sockets->head;
                        }
                        free(to_remove); // Liste node'unu serbest bırak
                        view_sockets->size--;
                        continue; // Bir sonraki node'a geç (v_node zaten güncellendi)
                    }
                }
            }
            v_prev = v_node;
            if (v_node)
                v_node = v_node->next;
        }
        pthread_mutex_unlock(&view_sockets->lock);
        json_object_put(state_jobj);
    }
    return NULL;
}

void *drone_accept_loop(void *arg)
{
    int server_fd = *(int *)arg;
    printf("Drone acceptor listening on port %d\n", PORT);
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int *client_sock_ptr = malloc(sizeof(int)); // handle_drone'a geçmek için
        if (!client_sock_ptr)
        {
            perror("malloc for client_sock_ptr failed");
            sleep(1); // Kısa bir süre bekle ve tekrar dene
            continue;
        }

        *client_sock_ptr = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (*client_sock_ptr < 0)
        {
            perror("accept for drone failed");
            free(client_sock_ptr); // Hata durumunda serbest bırak
            // Sunucu soketiyle ilgili ciddi bir sorun olabilir, logla ve devam et veya çık
            continue;
        }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("Accepted new drone connection from %s:%d (socket %d)\n", client_ip, ntohs(client_addr.sin_port), *client_sock_ptr);

        pthread_t drone_thread;
        if (pthread_create(&drone_thread, NULL, handle_drone, client_sock_ptr) != 0)
        {
            perror("pthread_create for handle_drone failed");
            close(*client_sock_ptr); // Soketi kapat
            free(client_sock_ptr);   // Malloc'u serbest bırak
        }
        else
        {
            pthread_detach(drone_thread); // Thread'in kendi kendine temizlenmesini sağla
        }
    }
    // Bu döngüden normalde çıkılmaz.
    free(arg); // server_fd için olan int* (main'den geldiyse)
    return NULL;
}

void *view_accept_loop(void *arg)
{
    int view_server_fd = *(int *)arg;
    printf("View acceptor listening on port %d\n", VIEW_PORT);
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int *view_client_sock_ptr = malloc(sizeof(int));
        if (!view_client_sock_ptr)
        {
            perror("malloc for view_client_sock_ptr failed");
            sleep(1);
            continue;
        }

        *view_client_sock_ptr = accept(view_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (*view_client_sock_ptr < 0)
        {
            perror("accept for view client failed");
            free(view_client_sock_ptr);
            continue;
        }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("Accepted new view connection from %s:%d (socket %d)\n", client_ip, ntohs(client_addr.sin_port), *view_client_sock_ptr);

        // Handshake burada yapılabilir veya handle_view_client içinde
        // Şimdilik listeye ekleyelim, handshake'i handle_view_client yapsın.
        // Eğer handshake başarısız olursa, handle_view_client soketi kapatır,
        // ve view_broadcast bir sonraki gönderimde listeden çıkarır.
        add_list(view_sockets, view_client_sock_ptr); // Listeye int* ekle

        pthread_t view_thread;
        // handle_view_client'a view_client_sock_ptr'ı doğrudan geçebiliriz.
        // handle_view_client'ın başında free(arg) yapmamalı.
        if (pthread_create(&view_thread, NULL, handle_view_client, view_client_sock_ptr) != 0)
        {
            perror("pthread_create for handle_view_client failed");
            close(*view_client_sock_ptr);
            // Listeden de çıkarmak gerekebilir, eğer eklendiyse
            remove_list(view_sockets, view_client_sock_ptr, compare_view_socket); // Ya da pointer adresiyle karşılaştır
            free(view_client_sock_ptr);
        }
        else
        {
            pthread_detach(view_thread);
        }
    }
    free(arg); // view_server_fd için olan int*
    return NULL;
}

int main()
{
    srand(time(NULL)); // Rastgele sayı üreteci için

    drone_list = create_list();
    survivor_list = create_list();
    view_sockets = create_list(); // View client soketlerini (int*) tutacak

    // Drone bağlantıları için sunucu soketi
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket for drones failed");
        return 1;
    }
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    // SO_REUSEADDR ayarı, sunucuyu yeniden başlatırken "Address already in use" hatasını önler
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) for drones failed");
    }
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind for drones failed");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, MAX_DRONES) < 0)
    {
        perror("listen for drones failed");
        close(server_fd);
        return 1;
    }
    printf("Drone server listening on port %d\n", PORT);

    // View bağlantıları için sunucu soketi
    int view_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (view_server_fd < 0)
    {
        perror("socket for views failed");
        close(server_fd);
        return 1;
    }
    struct sockaddr_in view_addr = {0};
    view_addr.sin_family = AF_INET;
    view_addr.sin_addr.s_addr = INADDR_ANY;
    view_addr.sin_port = htons(VIEW_PORT);
    if (setsockopt(view_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) for views failed");
    }
    if (bind(view_server_fd, (struct sockaddr *)&view_addr, sizeof(view_addr)) < 0)
    {
        perror("bind for views failed");
        close(server_fd);
        close(view_server_fd);
        return 1;
    }
    if (listen(view_server_fd, MAX_VIEWS) < 0)
    {
        perror("listen for views failed");
        close(server_fd);
        close(view_server_fd);
        return 1;
    }
    printf("View server listening on port %d\n", VIEW_PORT);

    pthread_t survivor_gen_thread, controller_thread, view_bcast_thread;
    pthread_t drone_accept_tid, view_accept_tid;

    // Çekirdek thread'leri başlat
    pthread_create(&survivor_gen_thread, NULL, survivor_generator, NULL);
    pthread_create(&controller_thread, NULL, controller, NULL);
    pthread_create(&view_bcast_thread, NULL, view_broadcast, NULL);

    // Bağlantı kabul thread'lerini başlat
    // Bu thread'lere soket tanımlayıcılarını (int) doğrudan geçmek yerine,
    // heap'te bir int* oluşturup onun adresini geçmek daha güvenli olabilir
    // veya sadece int değerini void* olarak cast edip thread içinde geri cast etmek.
    // Şimdilik main stack'indeki değişkenlerin adreslerini geçiyoruz, bu riskli olabilir
    // eğer main scope'u biterse. Ama main en son biten olacak.
    // Daha iyisi:
    int *p_server_fd = malloc(sizeof(int));
    *p_server_fd = server_fd;
    int *p_view_server_fd = malloc(sizeof(int));
    *p_view_server_fd = view_server_fd;

    pthread_create(&drone_accept_tid, NULL, drone_accept_loop, p_server_fd);
    pthread_create(&view_accept_tid, NULL, view_accept_loop, p_view_server_fd);

    // Thread'lerin ana thread'den ayrılması
    pthread_detach(survivor_gen_thread);
    pthread_detach(controller_thread);
    pthread_detach(view_bcast_thread);
    pthread_detach(drone_accept_tid);
    pthread_detach(view_accept_tid);

    // Ana thread burada bekleyebilir veya başka işler yapabilir.
    // Şimdilik sadece beklesin. Ctrl+C ile sonlandırılacak.
    printf("Server running. Press Ctrl+C to exit.\n");
    while (1)
    {
        sleep(60); // Ana thread'in meşgul olmasını önle
    }

    // Normalde buraya gelinmez (Ctrl+C ile çıkış varsayımı)
    // Ama düzgün kapatma için:
    printf("Shutting down server...\n");
    close(server_fd);
    close(view_server_fd);
    free(p_server_fd);
    free(p_view_server_fd);

    destroy_list(drone_list, free_drone);       // free_drone, Drone* alır
    destroy_list(survivor_list, free_survivor); // free_survivor, Survivor* alır
    destroy_list(view_sockets, free);           // view_sockets int* tutar, bu yüzden free yeterli

    printf("Server shut down complete.\n");
    return 0;
}