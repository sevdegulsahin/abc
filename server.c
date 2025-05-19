#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>     // YENİ: Sinyal yönetimi için
#include <time.h>       // YENİ: Zaman fonksiyonları için
#include <sys/select.h> // select, fd_set, FD_ZERO, FD_SET, FD_ISSET için
#include <sys/time.h>
#include "list.h"
#include "drone.h"
#include "survivor.h"
// #include "view.h" // Eğer view.h sadece view_thread prototipi içeriyorsa ve burada kullanılmıyorsa kaldırılabilir.

#define PORT 8080
#define VIEW_PORT 8081
#define MAX_DRONES 50
#define MAX_VIEWS 10
#define RECV_BUFFER_SIZE 4096
#define PROCESS_BUFFER_SIZE (RECV_BUFFER_SIZE * 2)

#define HEARTBEAT_INTERVAL 10 // YENİ: Saniye cinsinden heartbeat gönderme aralığı
#define DRONE_TIMEOUT 30      // YENİ: Saniye cinsinden drone'dan haber alınamazsa zaman aşımı

// Global değişkenler
List *drone_list;
List *survivor_list;
List *view_sockets;
volatile sig_atomic_t server_running = 1; // YENİ: Sunucunun çalışıp çalışmadığını kontrol eder

// Sinyal işleyici fonksiyonu
void signal_handler(int signum)
{
    printf("\nSignal %d received, shutting down server...\n", signum);
    server_running = 0;
}

int compare_drone_by_id_ptr(void *a, void *b)
{ // Karşılaştırma için ID'yi alır
    Drone *d1 = (Drone *)a;
    int id_b = *(int *)b;
    return d1->id - id_b;
}

int compare_drone_by_ptr(void *a, void *b)
{                                            // İki drone pointer'ını karşılaştırır
    return (Drone *)a == (Drone *)b ? 0 : 1; // Veya ID'lerine göre de olabilir: return ((Drone*)a)->id - ((Drone*)b)->id;
}

int compare_survivor_by_id_ptr(void *a, void *b)
{
    Survivor *s1 = (Survivor *)a;
    int id_b = *(int *)b;
    return s1->id - id_b;
}

void send_json_to_socket(int sock, json_object *jobj)
{
    if (sock <= 0 || !jobj)
        return;
    const char *str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    if (!str)
    {
        fprintf(stderr, "Error: json_object_to_json_string_ext failed.\n");
        return;
    }
    // MSG_NOSIGNAL, yazma işlemi sırasında soket aniden kapanırsa SIGPIPE sinyalini önler.
    if (send(sock, str, strlen(str), MSG_NOSIGNAL) < 0)
    {
        // perror("send_json: send string failed"); // Çok fazla log üretebilir
    }
    if (send(sock, "\n", 1, MSG_NOSIGNAL) < 0)
    {
        // perror("send_json: send newline failed");
    }
}

// YENİ: Bir survivor'ın hedefini kaldırmak için yardımcı fonksiyon
void unassign_survivor_target(Coordinate target_coord)
{
    pthread_mutex_lock(&survivor_list->lock);
    Node *s_node = survivor_list->head;
    while (s_node)
    {
        Survivor *s = (Survivor *)s_node->data;
        if (s->coord.x == target_coord.x && s->coord.y == target_coord.y && s->is_targeted)
        {
            s->is_targeted = false;
            printf("INFO: Survivor S%d at (%d,%d) is now unassigned due to drone issue.\n",
                   s->id, s->coord.x, s->coord.y);
            break; // Genellikle bir hedefe sadece bir survivor atanır
        }
        s_node = s_node->next;
    }
    pthread_mutex_unlock(&survivor_list->lock);
}

void *handle_drone(void *arg)
{
    int sock = *(int *)arg;
    free(arg);

    char recv_buffer[RECV_BUFFER_SIZE];
    char process_buffer[PROCESS_BUFFER_SIZE] = {0};
    int process_buffer_len = 0;
    Drone *drone_obj = NULL; // Bu thread'e ait drone nesnesi
    time_t last_heartbeat_sent_time = time(NULL);

    printf("Drone handler thread started for socket %d\n", sock);

    // select için zaman aşımı ayarı
    struct timeval tv;
    fd_set read_fds;

    while (server_running && sock > 0)
    {
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        tv.tv_sec = 1; // 1 saniye timeout (heartbeat ve server_running kontrolü için)
        tv.tv_usec = 0;

        int activity = select(sock + 1, &read_fds, NULL, NULL, &tv);

        if (activity < 0 && errno != EINTR)
        { // EINTR sinyal kesmesidir, hata değil
            perror("select error in handle_drone");
            break;
        }

        if (!server_running)
            break; // Sunucu kapanıyor

        // Heartbeat gönderme zamanı geldi mi? (Sadece drone_obj oluşturulduktan sonra)
        if (drone_obj && difftime(time(NULL), last_heartbeat_sent_time) >= HEARTBEAT_INTERVAL)
        {
            json_object *hb_jobj = json_object_new_object();
            json_object_object_add(hb_jobj, "type", json_object_new_string("HEARTBEAT"));
            // printf("Server: Sending HEARTBEAT to Drone D%d (sock %d)\n", drone_obj->id, sock);
            send_json_to_socket(sock, hb_jobj);
            json_object_put(hb_jobj);
            last_heartbeat_sent_time = time(NULL);
        }

        if (FD_ISSET(sock, &read_fds))
        { // Sokette veri var
            int len = recv(sock, recv_buffer, sizeof(recv_buffer) - 1, 0);
            if (len <= 0)
            {
                if (len == 0)
                    printf("Drone disconnected (socket: %d)\n", sock);
                else
                    perror("recv failed for drone socket");
                break;
            }
            recv_buffer[len] = '\0';

            if (process_buffer_len + len < PROCESS_BUFFER_SIZE)
            {
                memcpy(process_buffer + process_buffer_len, recv_buffer, len);
                process_buffer_len += len;
                process_buffer[process_buffer_len] = '\0';
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

            while ((msg_end = strchr(msg_start, '\n')) != NULL)
            {
                *msg_end = '\0';
                json_object *jobj = json_tokener_parse(msg_start);
                if (!jobj)
                {
                    fprintf(stderr, "Failed to parse JSON from drone socket %d: %s\n", sock, msg_start);
                    msg_start = msg_end + 1;
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

                // Drone'dan bir mesaj geldi, son mesaj zamanını güncelle
                if (drone_obj)
                {
                    pthread_mutex_lock(&drone_obj->lock);
                    drone_obj->last_message_time = time(NULL);
                    pthread_mutex_unlock(&drone_obj->lock);
                }

                if (strcmp(type_str, "HANDSHAKE") == 0)
                {
                    const char *drone_id_json = json_object_get_string(json_object_object_get(jobj, "drone_id"));
                    if (!drone_id_json)
                    { /* Hata işleme */
                        msg_start = msg_end + 1;
                        json_object_put(jobj);
                        continue;
                    }

                    char id_str_buf[20];
                    strncpy(id_str_buf, drone_id_json, sizeof(id_str_buf) - 1);
                    id_str_buf[sizeof(id_str_buf) - 1] = '\0';
                    char *id_ptr = id_str_buf;
                    if (id_ptr[0] == 'D' || id_ptr[0] == 'd')
                        id_ptr++;
                    int id = atoi(id_ptr);

                    bool exists = false;
                    pthread_mutex_lock(&drone_list->lock);
                    Node *temp_node = drone_list->head;
                    while (temp_node)
                    {
                        Drone *existing_drone = (Drone *)temp_node->data;
                        if (existing_drone->id == id)
                        {
                            exists = true;
                            break;
                        }
                        temp_node = temp_node->next;
                    }
                    pthread_mutex_unlock(&drone_list->lock);

                    if (exists)
                    {
                        printf("Drone D%d (socket %d) already connected. Closing new connection.\n", id, sock);
                        json_object_put(jobj);
                        // Yinelenen bağlantı için ACK göndermeden kapat
                        close(sock);
                        sock = -1; // Bu thread'in sonlanması için
                        // drone_obj NULL kalacak ve temizlikte sorun olmayacak
                        msg_start = msg_end + 1; // Sonraki mesaja geç (gerçi bağlantı kapanacak)
                        // Döngüden çıkmak için break;
                        goto cleanup_and_exit; // Daha temiz
                    }

                    drone_obj = create_drone(id, -1, -1);
                    if (!drone_obj)
                    { /* Hata işleme */
                        msg_start = msg_end + 1;
                        json_object_put(jobj);
                        continue;
                    }
                    drone_obj->sock = sock;
                    pthread_mutex_lock(&drone_obj->lock); // last_message_time için
                    drone_obj->last_message_time = time(NULL);
                    pthread_mutex_unlock(&drone_obj->lock);

                    add_list(drone_list, drone_obj);
                    printf("Drone D%d (socket %d) connected. Handshake successful.\n", id, sock);

                    json_object *ack = json_object_new_object();
                    json_object_object_add(ack, "type", json_object_new_string("HANDSHAKE_ACK"));
                    // İsteğe bağlı: sunucu kapasitesi, harita boyutu vb. bilgiler eklenebilir.
                    send_json_to_socket(sock, ack);
                    json_object_put(ack);
                }
                else if (drone_obj && strcmp(type_str, "STATUS_UPDATE") == 0)
                {
                    pthread_mutex_lock(&drone_obj->lock);
                    json_object *loc_obj = json_object_object_get(jobj, "location");
                    if (loc_obj)
                    {
                        drone_obj->coord.x = json_object_get_int(json_object_object_get(loc_obj, "x"));
                        drone_obj->coord.y = json_object_get_int(json_object_object_get(loc_obj, "y"));
                    }
                    const char *status_str = json_object_get_string(json_object_object_get(jobj, "status"));
                    if (status_str)
                    {
                        drone_obj->status = (strcmp(status_str, "idle") == 0) ? IDLE : ON_MISSION;
                    }
                    drone_obj->battery = json_object_get_int(json_object_object_get(jobj, "battery"));
                    // printf("Drone D%d status: (%d,%d), %s, Bat: %d\n", drone_obj->id, drone_obj->coord.x, drone_obj->coord.y, drone_obj->status == IDLE ? "IDLE" : "ON_MISSION", drone_obj->battery);
                    pthread_mutex_unlock(&drone_obj->lock);
                }
                else if (drone_obj && strcmp(type_str, "MISSION_COMPLETE") == 0)
                {
                    Coordinate completed_mission_target = {-1, -1}; // GÜNCELLENDİ: Başlangıç değeri
                    pthread_mutex_lock(&drone_obj->lock);
                    drone_obj->status = IDLE;
                    // Hangi görevin tamamlandığı bilgisi client'tan gelmeli
                    json_object *completed_target_obj = json_object_object_get(jobj, "completed_target");
                    if (completed_target_obj)
                    {
                        completed_mission_target.x = json_object_get_int(json_object_object_get(completed_target_obj, "x"));
                        completed_mission_target.y = json_object_get_int(json_object_object_get(completed_target_obj, "y"));
                        printf("Drone D%d reported MISSION_COMPLETE for its target (%d,%d). Current pos: (%d,%d)\n",
                               drone_obj->id, completed_mission_target.x, completed_mission_target.y, drone_obj->coord.x, drone_obj->coord.y);
                    }
                    else
                    {
                        // Eski davranış: drone'un mevcut hedefi
                        completed_mission_target = drone_obj->target;
                        printf("Drone D%d reported MISSION_COMPLETE (target from drone state: %d,%d). Current pos: (%d,%d)\n",
                               drone_obj->id, completed_mission_target.x, completed_mission_target.y, drone_obj->coord.x, drone_obj->coord.y);
                    }
                    pthread_mutex_unlock(&drone_obj->lock);

                    if (completed_mission_target.x != -1)
                    { // Geçerli bir hedef varsa
                        bool survivor_found_and_removed = false;
                        pthread_mutex_lock(&survivor_list->lock);
                        Node *current_s_node = survivor_list->head;
                        Node *prev_s_node = NULL;
                        while (current_s_node != NULL)
                        {
                            Survivor *s = (Survivor *)current_s_node->data;
                            if (s->coord.x == completed_mission_target.x && s->coord.y == completed_mission_target.y)
                            {
                                printf("Survivor S%d at (%d,%d) rescued by drone D%d. Removing from list.\n",
                                       s->id, s->coord.x, s->coord.y, drone_obj->id);
                                if (prev_s_node == NULL)
                                    survivor_list->head = current_s_node->next;
                                else
                                    prev_s_node->next = current_s_node->next;

                                Node *to_free = current_s_node;
                                current_s_node = current_s_node->next;
                                free_survivor(to_free->data);
                                free(to_free);
                                survivor_list->size--;
                                survivor_found_and_removed = true;
                                break;
                            }
                            prev_s_node = current_s_node;
                            current_s_node = current_s_node->next;
                        }
                        pthread_mutex_unlock(&survivor_list->lock);
                        if (!survivor_found_and_removed)
                        {
                            printf("Warning: Drone D%d completed mission at target (%d,%d), but no matching survivor found or already removed.\n",
                                   drone_obj->id, completed_mission_target.x, completed_mission_target.y);
                        }
                    }
                    // Yeni görev atama mantığı controller thread'ine bırakıldı.
                    // Controller periyodik olarak IDLE drone'ları kontrol edecek.
                }
                else if (drone_obj && strcmp(type_str, "BATTERY_DEPLETED") == 0)
                {
                    printf("Drone D%d battery depleted. (Socket %d)\n", drone_obj->id, sock);
                    pthread_mutex_lock(&drone_obj->lock);
                    if (drone_obj->status == ON_MISSION)
                    {
                        unassign_survivor_target(drone_obj->target); // GÜNCELLENDİ
                    }
                    drone_obj->status = IDLE; // Artık bir şey yapamaz
                    // drone_obj->sock = 0; // Bu, drone_obj'nin listeden çıkarılmasına yol açacak
                    pthread_mutex_unlock(&drone_obj->lock);
                    json_object_put(jobj);
                    goto cleanup_and_exit; // Temizle ve çık
                }
                else if (drone_obj && strcmp(type_str, "HEARTBEAT_ACK") == 0)
                { // YENİ
                  // printf("Drone D%d sent HEARTBEAT_ACK.\n", drone_obj->id);
                  // last_message_time zaten her mesajda güncelleniyor.
                }
                else if (drone_obj)
                {
                    printf("Drone D%d (socket %d) sent unknown/unhandled message type: %s\n", drone_obj->id, sock, type_str);
                }
                else
                { // drone_obj NULL ise (Handshake öncesi)
                    printf("Received message type '%s' from socket %d before handshake completed.\n", type_str, sock);
                }

                json_object_put(jobj);
                msg_start = msg_end + 1;
            }

            if (msg_start != process_buffer && *msg_start != '\0')
            {
                size_t remaining_len = process_buffer_len - (msg_start - process_buffer);
                memmove(process_buffer, msg_start, remaining_len);
                process_buffer_len = remaining_len;
                process_buffer[process_buffer_len] = '\0';
            }
            else if (*msg_start == '\0')
            {
                process_buffer[0] = '\0';
                process_buffer_len = 0;
            }
        }
        // select'ten sonra ve veri işlendikten sonra buraya gelinir.
        // Zaman aşımı kontrolü controller thread'inde yapılacak.
    }

cleanup_and_exit: // GÜNCELLENDİ: Temiz çıkış için goto etiketi
    printf("Drone handler for socket %d is terminating.\n", sock);
    if (drone_obj)
    {
        printf("Cleaning up for drone D%d (socket %d).\n", drone_obj->id, sock);
        pthread_mutex_lock(&drone_obj->lock);
        if (drone_obj->status == ON_MISSION)
        { // GÜNCELLENDİ: Bağlantı koparsa görevi iptal et
            unassign_survivor_target(drone_obj->target);
        }
        pthread_mutex_unlock(&drone_obj->lock); // Kilidi bırak

        pthread_mutex_lock(&drone_list->lock);
        // remove_list data pointer'ına göre karşılaştırma yapacak şekilde ayarlanmalı
        // veya ID'ye göre arama yapan bir remove_list_by_id fonksiyonu olmalı.
        // Mevcut remove_list(list, data, compare_func) için compare_func (void* a, void* b)'yi alır.
        // drone_obj pointer'ı ile listedeki drone'u bulup çıkarmak için özel bir compare fonksiyonu:
        remove_list(drone_list, drone_obj, compare_drone_by_ptr);
        pthread_mutex_unlock(&drone_list->lock);

        // drone_obj->sock zaten bu thread'in `sock` değişkeni.
        // free_drone içinde mutex_destroy var, kilidi tutarken çağırma.
        // free_drone'dan önce soketi kapatmak iyi bir pratik.
        if (drone_obj->sock > 0)
        { // Soket zaten kapatılmadıysa (örn. yinelenen bağlantı durumu)
          // close(drone_obj->sock); // close(sock) aşağıda zaten var.
        }
        drone_obj->sock = -1; // Artık geçersiz
        free_drone(drone_obj);
        drone_obj = NULL;
    }

    if (sock > 0)
        close(sock);
    return NULL;
}

void *controller(void *arg)
{
    (void)arg;
    while (server_running)
    {
        sleep(2); // Kontrol periyodu

        // 1. Zaman aşımına uğrayan drone'ları kontrol et ve çıkar
        pthread_mutex_lock(&drone_list->lock);
        Node *d_node_timeout = drone_list->head;
        Node *d_prev_timeout = NULL;
        time_t current_time = time(NULL);

        while (d_node_timeout)
        {
            Drone *d = (Drone *)d_node_timeout->data;
            bool remove_this_drone = false;

            pthread_mutex_lock(&d->lock);
            if (difftime(current_time, d->last_message_time) > DRONE_TIMEOUT)
            {
                printf("Drone D%d (socket %d) timed out. Last message: %.0f s ago. Removing.\n",
                       d->id, d->sock, difftime(current_time, d->last_message_time));
                remove_this_drone = true;
                if (d->status == ON_MISSION)
                {
                    // Hedefi serbest bırakma işlemi drone'un kendi lock'u dışarısında yapılmalı (survivor_list lock'u için)
                    // Ancak hedef bilgisini burada alalım.
                    Coordinate timed_out_target = d->target;
                    pthread_mutex_unlock(&d->lock);             // Drone kilidini bırak
                    unassign_survivor_target(timed_out_target); // Şimdi survivor hedefini kaldır
                }
                else
                {
                    pthread_mutex_unlock(&d->lock); // Drone kilidini bırak
                }

                if (d->sock > 0)
                { // Soketi kapat
                    close(d->sock);
                    d->sock = -1; // Gelecekte kullanılmaması için
                }
            }
            else
            {
                pthread_mutex_unlock(&d->lock);
            }

            if (remove_this_drone)
            {
                Node *temp_node_to_free = d_node_timeout;
                if (d_prev_timeout == NULL)
                { // Listenin başı
                    drone_list->head = d_node_timeout->next;
                    d_node_timeout = drone_list->head;
                }
                else
                {
                    d_prev_timeout->next = d_node_timeout->next;
                    d_node_timeout = d_prev_timeout->next;
                }
                drone_list->size--;
                free_drone(temp_node_to_free->data); // Drone nesnesini serbest bırak
                free(temp_node_to_free);             // Liste düğümünü serbest bırak
                // d_node_timeout zaten güncellendi, d_prev_timeout aynı kalır
            }
            else
            {
                d_prev_timeout = d_node_timeout;
                d_node_timeout = d_node_timeout->next;
            }
        }
        pthread_mutex_unlock(&drone_list->lock);

        // 2. Boştaki drone'lara görev ata
        pthread_mutex_lock(&drone_list->lock);
        Node *d_node_assign = drone_list->head;
        while (d_node_assign)
        {
            Drone *d = (Drone *)d_node_assign->data;
            pthread_mutex_lock(&d->lock);

            // Sadece IDLE, pili olan ve hala bağlı olan drone'ları değerlendir
            if (d->status == IDLE && d->battery > 0 && d->sock > 0)
            {
                Coordinate drone_current_pos = d->coord; // Pozisyonu kilit altındayken al
                pthread_mutex_unlock(&d->lock);          // Survivor ararken drone kilidini serbest bırak

                Survivor *best_survivor_to_assign = NULL;
                // int min_dist_for_best = -1; // GÜNCELLENDİ: Skorlama kullanılacak
                double max_score = -1.0; // En iyi skoru bulmak için

                pthread_mutex_lock(&survivor_list->lock);
                Node *s_node = survivor_list->head;
                time_t now = time(NULL);
                while (s_node)
                {
                    Survivor *s = (Survivor *)s_node->data;
                    if (!s->is_targeted)
                    {
                        int dist = abs(drone_current_pos.x - s->coord.x) +
                                   abs(drone_current_pos.y - s->coord.y);
                        time_t age = now - s->creation_time; // Yaş (saniye cinsinden)

                        // Basit bir skorlama: Yüksek öncelik, daha yaşlı, daha yakın olan daha iyi.
                        // Ağırlıkları ayarlayarak stratejiyi değiştirebilirsiniz.
                        double score = (s->priority * 100.0) + (age * 1.0) - (dist * 2.0);
                        // Örneğin: priority 3 ise +300, her saniye yaş için +1, her birim mesafe için -2 puan.

                        if (best_survivor_to_assign == NULL || score > max_score)
                        {
                            max_score = score;
                            best_survivor_to_assign = s;
                        }
                    }
                    s_node = s_node->next;
                }

                if (best_survivor_to_assign)
                {
                    pthread_mutex_lock(&d->lock); // Drone'a atama yapmak için kilidi tekrar al
                    // Son bir kontrol: Drone hala IDLE, pili var ve bağlı mı?
                    if (d->status == IDLE && d->battery > 0 && d->sock > 0)
                    {
                        d->status = ON_MISSION;
                        d->target = best_survivor_to_assign->coord;
                        best_survivor_to_assign->is_targeted = true;

                        json_object *mission_jobj = json_object_new_object();
                        json_object_object_add(mission_jobj, "type", json_object_new_string("ASSIGN_MISSION"));
                        char mission_id_str[50]; // Daha uzun mission_id için
                        snprintf(mission_id_str, sizeof(mission_id_str), "M_Ctrl_D%dS%d_T%ld",
                                 d->id, best_survivor_to_assign->id, (long)time(NULL));
                        json_object_object_add(mission_jobj, "mission_id", json_object_new_string(mission_id_str));
                        json_object *target_loc_jobj = json_object_new_object();
                        json_object_object_add(target_loc_jobj, "x", json_object_new_int(best_survivor_to_assign->coord.x));
                        json_object_object_add(target_loc_jobj, "y", json_object_new_int(best_survivor_to_assign->coord.y));
                        json_object_object_add(mission_jobj, "target", target_loc_jobj);
                        send_json_to_socket(d->sock, mission_jobj);
                        json_object_put(mission_jobj);

                        printf("Controller: Assigned drone D%d to survivor S%d (Prio:%d, Age:%lds, Dist:%d, Score:%.2f) at (%d,%d).\n",
                               d->id, best_survivor_to_assign->id, best_survivor_to_assign->priority,
                               (long)(now - best_survivor_to_assign->creation_time),
                               abs(drone_current_pos.x - best_survivor_to_assign->coord.x) + abs(drone_current_pos.y - best_survivor_to_assign->coord.y),
                               max_score,
                               best_survivor_to_assign->coord.x, best_survivor_to_assign->coord.y);
                    }
                    else
                    {
                        // Atama sırasında drone durumu değişmiş, survivor'ı serbest bırak
                        if (best_survivor_to_assign)
                            best_survivor_to_assign->is_targeted = false;
                    }
                    pthread_mutex_unlock(&d->lock);
                }
                pthread_mutex_unlock(&survivor_list->lock);
            }
            else
            { // Drone ON_MISSION, pili bitik veya soket kapalı
                pthread_mutex_unlock(&d->lock);
            }
            d_node_assign = d_node_assign->next;
        }
        pthread_mutex_unlock(&drone_list->lock);
    }
    printf("Controller thread exiting.\n");
    return NULL;
}

void *survivor_generator(void *arg)
{
    (void)arg;
    static int survivor_id_counter = 1; // 1'den başlasın
    while (server_running)
    {
        // Yeni survivor üretme sıklığı (server_running kontrolü için daha kısa sleep)
        for (int i = 0; i < 5 && server_running; ++i)
        { // 5 saniye bekle, her saniye kontrol et
            sleep(1);
        }
        if (!server_running)
            break;

        int x = rand() % 40;
        int y = rand() % 60;
        int priority = (rand() % 3) + 1; // 1, 2, veya 3

        Survivor *s = create_survivor(survivor_id_counter++, x, y, priority);
        if (s)
        {
            add_list(survivor_list, s);
            printf("Generated survivor S%d (Prio:%d) at (%d,%d).\n", s->id, s->priority, x, y);
        }
    }
    printf("Survivor generator thread exiting.\n");
    return NULL;
}

int compare_view_socket_ptr(void *a, void *b)
{
    return (*(int *)a == *(int *)b) ? 0 : 1; // Değerleri karşılaştır
}

void *handle_view_client(void *arg)
{
    int *sock_ptr_in_list = (int *)arg; // GÜNCELLENDİ: Argüman artık listedeki int*'ın kendisi
    int sock = *sock_ptr_in_list;       // Değeri al

    printf("View client handler started for socket %d\n", sock);
    char buffer[1024];
    bool handshake_done = false;

    // select için
    fd_set read_fds_view;
    struct timeval tv_view;

    // İlk mesajın handshake olmasını bekle (kısa bir timeout ile)
    FD_ZERO(&read_fds_view);
    FD_SET(sock, &read_fds_view);
    tv_view.tv_sec = 5; // 5 saniye handshake timeout
    tv_view.tv_usec = 0;

    int activity = select(sock + 1, &read_fds_view, NULL, NULL, &tv_view);

    if (activity > 0 && FD_ISSET(sock, &read_fds_view))
    {
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len > 0)
        {
            buffer[len] = '\0';
            char *newline = strchr(buffer, '\n');
            if (newline)
                *newline = '\0';

            json_object *jobj = json_tokener_parse(buffer);
            if (jobj)
            {
                const char *type = json_object_get_string(json_object_object_get(jobj, "type"));
                if (type && strcmp(type, "VIEW_HANDSHAKE") == 0)
                {
                    handshake_done = true;
                    printf("View client (socket %d) handshake successful.\n", sock);
                    json_object *ack = json_object_new_object();
                    json_object_object_add(ack, "type", json_object_new_string("VIEW_HANDSHAKE_ACK"));
                    send_json_to_socket(sock, ack);
                    json_object_put(ack);
                }
                else
                {
                    printf("View client (socket %d) sent non-handshake or invalid type. Closing.\n", sock);
                }
                json_object_put(jobj);
            }
            else
            {
                printf("View client (socket %d) sent invalid JSON for handshake. Closing.\n", sock);
            }
        }
        else
        { // len <= 0
            printf("View client (socket %d) disconnected before handshake or recv error.\n", sock);
        }
    }
    else if (activity == 0)
    { // Timeout
        printf("View client (socket %d) timed out waiting for handshake. Closing.\n", sock);
    }
    else
    { // select error
        perror("select error in handle_view_client handshake");
    }

    if (!handshake_done)
    {
        // Listeden çıkarma view_broadcast'e veya ana kapatmaya bırakılacak
        // ya da burada da yapılabilir. Şimdilik soketi kapat.
        close(sock);
        // `sock_ptr_in_list` view_broadcast'te veya destroy_list'te free edilecek.
        // Eğer burada çıkarılacaksa:
        // pthread_mutex_lock(&view_sockets->lock);
        // remove_list(view_sockets, sock_ptr_in_list, compare_exact_pointer_address); // Pointer adresine göre
        // pthread_mutex_unlock(&view_sockets->lock);
        // free(sock_ptr_in_list); // Eğer remove_list bunu free etmiyorsa
        return NULL;
    }

    // Handshake başarılı, bağlantının açık kalıp kalmadığını kontrol et.
    while (server_running && sock > 0)
    {
        FD_ZERO(&read_fds_view);
        FD_SET(sock, &read_fds_view);
        tv_view.tv_sec = 1; // Periyodik kontrol
        tv_view.tv_usec = 0;

        activity = select(sock + 1, &read_fds_view, NULL, NULL, &tv_view);
        if (!server_running)
            break;

        if (activity < 0 && errno != EINTR)
        {
            perror("recv error from view client (select)");
            break;
        }
        if (activity > 0 && FD_ISSET(sock, &read_fds_view))
        {
            int len = recv(sock, buffer, sizeof(buffer) - 1, MSG_DONTWAIT); // Non-blocking
            if (len == 0)
            {
                printf("View client (socket %d) disconnected.\n", sock);
                break;
            }
            if (len < 0)
            {
                if (errno != EWOULDBLOCK && errno != EAGAIN)
                {
                    perror("recv error from view client");
                    break;
                }
                // EWOULDBLOCK/EAGAIN ise veri yok, normal.
            }
            else if (len > 0)
            { // Beklenmedik veri
                buffer[len] = '\0';
                printf("Unexpected data from view client (socket %d): %s\n", sock, buffer);
            }
        }
    }

    printf("View client handler for socket %d terminating.\n", sock);
    // Soketi kapat, listeden çıkarma işlemini view_broadcast veya ana destroy_list yapar.
    // close(sock); // view_broadcast'in send hatasında kapatması daha iyi.
    // Eğer bu thread sonlanıyorsa ve view_broadcast hala o soketi kullanıyorsa sorun olabilir.
    // En iyisi view_broadcast'in send hatasında ele alması.
    // Ama bu thread bittiğinde soket hala listedeyse ve view_broadcast onu kullanmaya çalışırsa?
    // Bu yüzden, bu thread çıkarken listeden çıkarmak daha güvenli olabilir.
    pthread_mutex_lock(&view_sockets->lock);
    // `compare_view_socket_ptr` listedeki `int*`'ların *değerlerini* karşılaştırır.
    // Biz ise `sock_ptr_in_list` adresinin kendisini listeden çıkarmak istiyoruz.
    // Bu yüzden `compare_exact_pointer` gibi bir fonksiyona ihtiyaç var.
    // Veya `remove_list` içinde `data == node->data` şeklinde basit bir karşılaştırma yapılır.
    // Şimdilik, `view_broadcast`teki send hatasıyla çıkarmaya güvenelim.
    // Ya da `sock_ptr_in_list`'in değerini -1 yaparak `view_broadcast`'in onu atlamasını sağlayabiliriz.
    if (*sock_ptr_in_list > 0)
    {
        close(*sock_ptr_in_list);
        *sock_ptr_in_list = -1; // view_broadcast'in bu soketi atlaması için işaretle
    }
    pthread_mutex_unlock(&view_sockets->lock);

    return NULL;
}

void *view_broadcast(void *arg)
{
    (void)arg;
    while (server_running)
    {
        // Güncelleme sıklığı (server_running kontrolü için daha kısa sleep)
        for (int i = 0; i < 1 && server_running; ++i)
        { // 1 saniye bekle
            sleep(1);
        }
        if (!server_running)
            break;

        json_object *state_jobj = json_object_new_object();
        json_object_object_add(state_jobj, "type", json_object_new_string("STATE_UPDATE"));
        json_object_object_add(state_jobj, "timestamp", json_object_new_int64(time(NULL)));

        json_object *drones_arr = json_object_new_array();
        pthread_mutex_lock(&drone_list->lock);
        Node *d_node = drone_list->head;
        while (d_node)
        {
            Drone *d = (Drone *)d_node->data;
            pthread_mutex_lock(&d->lock);
            if (d->sock > 0)
            { // Sadece aktif soketi olan ve listeden çıkarılmamış dronelar
                json_object *d_obj = json_object_new_object();
                json_object_object_add(d_obj, "id", json_object_new_int(d->id));
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

        json_object *survivors_arr = json_object_new_array();
        pthread_mutex_lock(&survivor_list->lock);
        Node *s_node = survivor_list->head;
        while (s_node)
        {
            Survivor *s = (Survivor *)s_node->data;
            json_object *s_obj = json_object_new_object();
            json_object_object_add(s_obj, "id", json_object_new_int(s->id));
            json_object *loc = json_object_new_object();
            json_object_object_add(loc, "x", json_object_new_int(s->coord.x));
            json_object_object_add(loc, "y", json_object_new_int(s->coord.y));
            json_object_object_add(s_obj, "location", loc);
            json_object_object_add(s_obj, "priority", json_object_new_int(s->priority));
            json_object_object_add(s_obj, "is_targeted", json_object_new_boolean(s->is_targeted)); // GUI için
            json_object_array_add(survivors_arr, s_obj);
            s_node = s_node->next;
        }
        pthread_mutex_unlock(&survivor_list->lock);
        json_object_object_add(state_jobj, "survivors", survivors_arr);

        const char *json_str_payload = json_object_to_json_string_ext(state_jobj, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
        if (!json_str_payload)
        {
            fprintf(stderr, "View_broadcast: Failed to create JSON string.\n");
            json_object_put(state_jobj);
            continue;
        }

        pthread_mutex_lock(&view_sockets->lock);
        Node *v_node = view_sockets->head;
        Node *v_prev = NULL;
        while (v_node)
        {
            int *sock_ptr = (int *)v_node->data;
            bool remove_current_view_socket = false;

            if (sock_ptr && *sock_ptr > 0)
            { // Geçerli bir soket mi?
                if (send(*sock_ptr, json_str_payload, strlen(json_str_payload), MSG_NOSIGNAL) < 0 ||
                    send(*sock_ptr, "\n", 1, MSG_NOSIGNAL) < 0)
                {
                    // perror("send to view client failed"); // Çok fazla log üretebilir
                    printf("View client (socket %d) send error/disconnected, removing from broadcast list.\n", *sock_ptr);
                    close(*sock_ptr);
                    *sock_ptr = -1; // Artık geçersiz olarak işaretle (handle_view_client da bunu yapabilir)
                    remove_current_view_socket = true;
                }
            }
            else if (sock_ptr && *sock_ptr == -1)
            { // handle_view_client tarafından kapatılmış
                printf("View client (socket marked -1) found in list, removing.\n");
                remove_current_view_socket = true;
            }

            if (remove_current_view_socket)
            {
                Node *to_remove = v_node;
                if (v_prev)
                {
                    v_prev->next = v_node->next;
                    v_node = v_node->next; // v_node'u ilerlet, v_prev aynı kalsın
                }
                else
                { // Listenin başı
                    view_sockets->head = v_node->next;
                    v_node = view_sockets->head; // v_node'u ilerlet, v_prev NULL olacak
                }
                if (sock_ptr)
                    free(sock_ptr); // Malloc ile alınan int* 'ı serbest bırak
                free(to_remove);    // Liste node'unu serbest bırak
                view_sockets->size--;
                // v_node zaten güncellendi, döngüye devam et
            }
            else
            {
                v_prev = v_node;
                v_node = v_node->next;
            }
        }
        pthread_mutex_unlock(&view_sockets->lock);
        json_object_put(state_jobj); // Ana JSON nesnesini serbest bırak
    }
    printf("View broadcast thread exiting.\n");
    return NULL;
}

void *drone_accept_loop(void *arg)
{
    int server_fd = *(int *)arg; // server_fd değerini al, arg (p_server_fd) main'de free edilecek
    printf("Drone acceptor listening on port %d\n", PORT);

    // server_fd'yi non-blocking yapmak yerine select/poll ile accept edebiliriz
    // ya da shutdown sırasında server_fd'yi kapatarak accept'i sonlandırabiliriz.
    // Şimdilik server_running ile devam edelim.
    // accept'in timeout'lu olması için setsockopt ile SO_RCVTIMEO ayarlanabilir.

    struct timeval tv_accept;
    fd_set accept_fds;

    while (server_running)
    {
        FD_ZERO(&accept_fds);
        FD_SET(server_fd, &accept_fds);
        tv_accept.tv_sec = 1; // 1 saniye timeout ile accept beklemesi
        tv_accept.tv_usec = 0;

        int activity = select(server_fd + 1, &accept_fds, NULL, NULL, &tv_accept);

        if (activity < 0 && errno != EINTR)
        {
            if (!server_running)
                break; // Kapanış sırasında select hatası olabilir
            perror("select error in drone_accept_loop");
            continue;
        }

        if (!server_running)
            break;

        if (activity > 0 && FD_ISSET(server_fd, &accept_fds))
        {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int *client_sock_ptr = malloc(sizeof(int));
            if (!client_sock_ptr)
            {
                perror("malloc for client_sock_ptr failed in drone_accept");
                continue; // Veya sunucuyu kapat
            }

            *client_sock_ptr = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (*client_sock_ptr < 0)
            {
                if (!server_running && (errno == EBADF || errno == EINVAL))
                { // Sunucu soketi kapatılmış olabilir
                    free(client_sock_ptr);
                    break;
                }
                perror("accept for drone failed");
                free(client_sock_ptr);
                continue;
            }
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            printf("Accepted new drone connection from %s:%d (socket %d)\n", client_ip, ntohs(client_addr.sin_port), *client_sock_ptr);

            pthread_t drone_thread;
            if (pthread_create(&drone_thread, NULL, handle_drone, client_sock_ptr) != 0)
            {
                perror("pthread_create for handle_drone failed");
                close(*client_sock_ptr);
                free(client_sock_ptr);
            }
            else
            {
                pthread_detach(drone_thread);
            }
        }
    }
    printf("Drone accept loop exiting.\n");
    return NULL;
}

void *view_accept_loop(void *arg)
{
    int view_server_fd = *(int *)arg; // Değeri al
    printf("View acceptor listening on port %d\n", VIEW_PORT);

    struct timeval tv_accept_view;
    fd_set accept_fds_view;

    while (server_running)
    {
        FD_ZERO(&accept_fds_view);
        FD_SET(view_server_fd, &accept_fds_view);
        tv_accept_view.tv_sec = 1;
        tv_accept_view.tv_usec = 0;

        int activity = select(view_server_fd + 1, &accept_fds_view, NULL, NULL, &tv_accept_view);

        if (activity < 0 && errno != EINTR)
        {
            if (!server_running)
                break;
            perror("select error in view_accept_loop");
            continue;
        }
        if (!server_running)
            break;

        if (activity > 0 && FD_ISSET(view_server_fd, &accept_fds_view))
        {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int *view_client_sock_ptr = malloc(sizeof(int)); // Bu pointer handle_view_client'a geçilecek
            if (!view_client_sock_ptr)
            { // ve view_sockets listesinde saklanacak.
                perror("malloc for view_client_sock_ptr failed");
                continue;
            }

            *view_client_sock_ptr = accept(view_server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (*view_client_sock_ptr < 0)
            {
                if (!server_running && (errno == EBADF || errno == EINVAL))
                {
                    free(view_client_sock_ptr);
                    break;
                }
                perror("accept for view client failed");
                free(view_client_sock_ptr);
                continue;
            }
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            printf("Accepted new view connection from %s:%d (socket %d)\n", client_ip, ntohs(client_addr.sin_port), *view_client_sock_ptr);

            // Listeye eklemeden önce handle_view_client içinde handshake yaptırmak daha iyi olabilir.
            // Şimdilik handshake'i handle_view_client yapsın, eğer başarısız olursa orası soketi kapatır.
            // Listeye ekleme de orada yapılabilir veya burada yapılıp handshake başarısız olursa çıkarılır.
            // Daha basit: handle_view_client'a `view_client_sock_ptr`'ı ver, o listeye eklesin/çıkarsın.
            // Ya da burada listeye ekle, handle_view_client sadece işaretlesin/kapatsın, broadcast çıkarsın.

            // `view_client_sock_ptr`'ı listeye ekle, `handle_view_client` bu pointer'ı argüman olarak alacak.
            add_list(view_sockets, view_client_sock_ptr);

            pthread_t view_thread;
            // handle_view_client'a view_client_sock_ptr'ı (int** değil, int* yani malloc'lanmış adres) doğrudan geç.
            if (pthread_create(&view_thread, NULL, handle_view_client, view_client_sock_ptr) != 0)
            {
                perror("pthread_create for handle_view_client failed");
                close(*view_client_sock_ptr);
                // Listeden çıkarmak gerekebilir, eğer eklendiyse. `compare_view_socket_ptr` ile değeri arar.
                // Veya pointer adresiyle: `remove_list(view_sockets, view_client_sock_ptr, compare_exact_pointer_address);`
                // Şimdilik, view_broadcast'in temizlemesine güven.
                pthread_mutex_lock(&view_sockets->lock);
                remove_list(view_sockets, view_client_sock_ptr, compare_view_socket_ptr); // Değere göre arayıp çıkar
                pthread_mutex_unlock(&view_sockets->lock);
                free(view_client_sock_ptr); // `remove_list` datayı free etmiyorsa
            }
            else
            {
                pthread_detach(view_thread);
            }
        }
    }
    printf("View accept loop exiting.\n");
    return NULL;
}

int main()
{
    srand(time(NULL));

    // Sinyal işleyicilerini ayarla
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // SIGPIPE'ı yok saymak, send hatalarında programın çökmesini engeller.
    // Bunun yerine send hatalarını kontrol etmek daha iyi. MSG_NOSIGNAL kullanılır.
    // signal(SIGPIPE, SIG_IGN);

    drone_list = create_list();
    survivor_list = create_list();
    view_sockets = create_list();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // ... (socket, setsockopt, bind, listen for server_fd) ...
    if (server_fd < 0)
    {
        perror("socket for drones failed");
        return 1;
    }
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) for drones failed");
        close(server_fd);
        return 1;
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

    int view_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // ... (socket, setsockopt, bind, listen for view_server_fd) ...
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
        close(server_fd);
        close(view_server_fd);
        return 1;
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

    // Thread'lere geçmek için server_fd ve view_server_fd'nin kopyalarını heap'te oluştur
    int *p_server_fd = malloc(sizeof(int));
    if (!p_server_fd)
    {
        perror("malloc p_server_fd failed"); /* cleanup and exit */
        return 1;
    }
    *p_server_fd = server_fd;

    int *p_view_server_fd = malloc(sizeof(int));
    if (!p_view_server_fd)
    {
        perror("malloc p_view_server_fd failed");
        free(p_server_fd); /* cleanup */
        return 1;
    }
    *p_view_server_fd = view_server_fd;

    pthread_create(&survivor_gen_thread, NULL, survivor_generator, NULL);
    pthread_create(&controller_thread, NULL, controller, NULL);
    pthread_create(&view_bcast_thread, NULL, view_broadcast, NULL);
    pthread_create(&drone_accept_tid, NULL, drone_accept_loop, p_server_fd);
    pthread_create(&view_accept_tid, NULL, view_accept_loop, p_view_server_fd);

    printf("Server running. Press Ctrl+C to exit.\n");

    // Ana thread, server_running 0 olana kadar bekler
    while (server_running)
    {
        sleep(1); // CPU'yu yormamak için
    }

    // Kapanış işlemleri
    printf("Shutting down server components...\n");

    // 1. Yeni bağlantıları kabul etmeyi durdur (accept loop'lar server_running'i kontrol ediyor)
    //    ve ana dinleme soketlerini kapatarak accept'lerin sonlanmasını hızlandır.
    if (server_fd > 0)
        close(server_fd);
    if (view_server_fd > 0)
        close(view_server_fd);
    server_fd = -1; // Tekrar kapatılmasını önle
    view_server_fd = -1;

    // 2. Worker thread'lerin sonlanmasını bekle (join)
    //    Detach edilmemişlerse join edilebilir. Detach edilmişlerse,
    //    server_running flag'ini kontrol ederek kendiliğinden çıkmaları beklenir.
    //    handle_drone ve handle_view_client thread'leri detach edildiği için,
    //    onların soketlerinin kapatılması (veya timeout) çıkmalarını sağlar.

    printf("Waiting for survivor generator thread to exit...\n");
    pthread_join(survivor_gen_thread, NULL);
    printf("Waiting for controller thread to exit...\n");
    pthread_join(controller_thread, NULL);
    printf("Waiting for view broadcast thread to exit...\n");
    pthread_join(view_bcast_thread, NULL);
    printf("Waiting for drone accept loop to exit...\n");
    pthread_join(drone_accept_tid, NULL);
    printf("Waiting for view accept loop to exit...\n");
    pthread_join(view_accept_tid, NULL);

    // drone_accept_loop ve view_accept_loop'a geçilen p_server_fd ve p_view_server_fd'yi free et
    free(p_server_fd);
    free(p_view_server_fd);

    // handle_drone thread'leri detach edilmişti.
    // Onların kullandığı drone_obj'ler ve soketler,
    // drone_list temizlenirken veya handle_drone çıkarken temizlenmeli.
    // Zaman aşımı veya bağlantı kopması durumunda handle_drone zaten temizlik yapıyor.
    // Kapanışta, drone_list'teki tüm drone'ların soketleri kapatılabilir
    // (eğer hala açıksa) ve sonra liste temizlenir.
    printf("Cleaning up remaining drone connections...\n");
    pthread_mutex_lock(&drone_list->lock);
    Node *current_d_node = drone_list->head;
    while (current_d_node)
    {
        Drone *d = (Drone *)current_d_node->data;
        pthread_mutex_lock(&d->lock);
        if (d->sock > 0)
        {
            close(d->sock);
            d->sock = -1;
        }
        pthread_mutex_unlock(&d->lock);
        current_d_node = current_d_node->next;
    }
    pthread_mutex_unlock(&drone_list->lock);
    destroy_list(drone_list, free_drone); // free_drone, Drone* alır

    printf("Cleaning up remaining view connections...\n");
    pthread_mutex_lock(&view_sockets->lock);
    Node *current_v_node = view_sockets->head;
    while (current_v_node)
    {
        int *sock_val_ptr = (int *)current_v_node->data;
        if (sock_val_ptr && *sock_val_ptr > 0)
        {
            close(*sock_val_ptr);
            *sock_val_ptr = -1;
        }
        current_v_node = current_v_node->next;
    }
    pthread_mutex_unlock(&view_sockets->lock);
    destroy_list(view_sockets, free); // view_sockets int* tutar, bu yüzden free yeterli

    destroy_list(survivor_list, free_survivor);

    printf("Server shut down complete.\n");
    return 0;
}