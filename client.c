#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <time.h> // YENİ: time() için
#include "drone.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8080
#define PROCESS_BUFFER_SIZE (4096 * 2) // GÜNCELLENDİ: Buffer boyutu

int connect_to_server()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connect failed");
        return -1;
    }
    return sock;
}

void send_json(int sock, json_object *jobj)
{
    if (sock <= 0)
        return;
    const char *str = json_object_to_json_string(jobj);
    send(sock, str, strlen(str), 0);
    send(sock, "\n", 1, 0); // Mesaj ayırıcı
}

void *navigate_to_target(void *arg)
{
    Drone *d = (Drone *)arg;
    int sock = d->sock;
    int move_count = 0;

    while (d->sock > 0) // GÜNCELLENDİ: sock kontrolü
    {
        pthread_mutex_lock(&d->lock);
        if (d->battery <= 0)
        {
            d->status = IDLE; // Pili bitti, boşta
            printf("Drone %d: Battery depleted, stopping mission.\n", d->id);
            json_object *jobj = json_object_new_object();
            json_object_object_add(jobj, "type", json_object_new_string("BATTERY_DEPLETED"));
            char drone_id_str[10];
            snprintf(drone_id_str, sizeof(drone_id_str), "D%d", d->id);
            json_object_object_add(jobj, "drone_id", json_object_new_string(drone_id_str));
            json_object_object_add(jobj, "timestamp", json_object_new_int64(time(NULL))); // time_t için int64
            send_json(sock, jobj);
            json_object_put(jobj);
            pthread_mutex_unlock(&d->lock);
            // Pili biten drone'un thread'i burada sonlanabilir veya ana döngüde handle edilir.
            // Şimdilik döngüyü kırıp thread'in sonlanmasını sağlıyoruz.
            d->sock = 0; // Ana döngünün de sonlanması için
            break;
        }

        if (d->status == ON_MISSION)
        {
            int moved = 0;
            if (d->coord.x < d->target.x)
            {
                d->coord.x++;
                moved = 1;
            }
            else if (d->coord.x > d->target.x)
            {
                d->coord.x--;
                moved = 1;
            }

            // Sadece X ekseninde hedefteyse Y ekseninde hareket et
            if (d->coord.x == d->target.x)
            {
                if (d->coord.y < d->target.y)
                {
                    d->coord.y++;
                    moved = 1;
                }
                else if (d->coord.y > d->target.y)
                {
                    d->coord.y--;
                    moved = 1;
                }
            }

            if (moved)
            {
                move_count++;
                if (move_count >= 5)
                {
                    d->battery--;
                    move_count = 0;
                }
            }

            // printf("Drone %d at (%d, %d), moving to (%d, %d), battery: %d\n",
            //        d->id, d->coord.x, d->coord.y, d->target.x, d->target.y, d->battery);

            if (d->coord.x == d->target.x && d->coord.y == d->target.y)
            {
                d->status = IDLE;
                printf("Drone %d: Mission completed at (%d, %d)\n", d->id, d->target.x, d->target.y);
                json_object *jobj = json_object_new_object();
                json_object_object_add(jobj, "type", json_object_new_string("MISSION_COMPLETE"));
                char drone_id_str[10];
                snprintf(drone_id_str, sizeof(drone_id_str), "D%d", d->id);
                json_object_object_add(jobj, "drone_id", json_object_new_string(drone_id_str));
                // mission_id sunucudan gelmeli, şimdilik sabit veya yok.
                // json_object_object_add(jobj, "mission_id", json_object_new_string("M123"));
                json_object_object_add(jobj, "timestamp", json_object_new_int64(time(NULL)));
                json_object_object_add(jobj, "success", json_object_new_boolean(true));
                json_object *completed_target_loc = json_object_new_object(); // YENİ: Hangi görevin tamamlandığı
                json_object_object_add(completed_target_loc, "x", json_object_new_int(d->target.x));
                json_object_object_add(completed_target_loc, "y", json_object_new_int(d->target.y));
                json_object_object_add(jobj, "completed_target", completed_target_loc);
                send_json(sock, jobj);
                json_object_put(jobj);
            }
        }
        pthread_mutex_unlock(&d->lock);
        sleep(1); // Saniyede 1 hareket
    }
    printf("Drone %d: Navigate thread exiting.\n", d->id);
    return NULL;
}

void *send_status_update(void *arg)
{
    Drone *d = (Drone *)arg;
    int sock = d->sock;
    while (d->sock > 0) // GÜNCELLENDİ: sock kontrolü
    {
        pthread_mutex_lock(&d->lock);
        if (d->sock <= 0)
        { // Ana döngüden çıkış sinyali
            pthread_mutex_unlock(&d->lock);
            break;
        }

        json_object *jobj = json_object_new_object();
        json_object_object_add(jobj, "type", json_object_new_string("STATUS_UPDATE"));
        char drone_id_str[10];
        snprintf(drone_id_str, sizeof(drone_id_str), "D%d", d->id);
        json_object_object_add(jobj, "drone_id", json_object_new_string(drone_id_str));
        json_object_object_add(jobj, "timestamp", json_object_new_int64(time(NULL)));
        json_object *loc = json_object_new_object();
        json_object_object_add(loc, "x", json_object_new_int(d->coord.x));
        json_object_object_add(loc, "y", json_object_new_int(d->coord.y));
        json_object_object_add(jobj, "location", loc);
        json_object_object_add(jobj, "status", json_object_new_string(d->status == IDLE ? "idle" : "busy"));
        json_object_object_add(jobj, "battery", json_object_new_int(d->battery));
        // json_object_object_add(jobj, "speed", json_object_new_int(5)); // İsteğe bağlı
        send_json(sock, jobj);
        json_object_put(jobj);

        pthread_mutex_unlock(&d->lock);
        // sleep(5); // Daha sık güncelleme, örn. 2 saniyede bir
        sleep(2);
    }
    printf("Drone %d: Status update thread exiting.\n", d->id);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Kullanım: %s <drone_id_sayisi>\nÖrnek: %s D1\n", argv[0], argv[0]);
        return 1;
    }

    int sock = connect_to_server();
    if (sock < 0)
        return 1;

    char *id_str_param = argv[1];
    if (id_str_param[0] == 'D' || id_str_param[0] == 'd')
    {
        id_str_param++; // 'D' harfini atla
    }
    int drone_id_val = atoi(id_str_param);
    if (drone_id_val <= 0)
    {
        fprintf(stderr, "Geçersiz drone ID: %s\n", argv[1]);
        close(sock);
        return 1;
    }

    Drone *drone = create_drone(drone_id_val, -1, -1); // ID'yi parametreden al
    drone->sock = sock;

    // HANDSHAKE mesajı gönder
    json_object *handshake = json_object_new_object();
    json_object_object_add(handshake, "type", json_object_new_string("HANDSHAKE"));
    char drone_id_json_str[10];
    snprintf(drone_id_json_str, sizeof(drone_id_json_str), "D%d", drone->id);
    json_object_object_add(handshake, "drone_id", json_object_new_string(drone_id_json_str));
    json_object *caps = json_object_new_object();
    json_object_object_add(caps, "max_speed", json_object_new_int(30));         // Örnek değer
    json_object_object_add(caps, "battery_capacity", json_object_new_int(100)); // Örnek değer
    json_object_object_add(caps, "payload", json_object_new_string("medical")); // Örnek değer
    json_object_object_add(handshake, "capabilities", caps);
    send_json(sock, handshake);
    json_object_put(handshake);

    pthread_t navigate_thread, status_thread;
    pthread_create(&navigate_thread, NULL, navigate_to_target, drone);
    pthread_create(&status_thread, NULL, send_status_update, drone);

    char recv_buffer[1024];                         // Gelen tekil paketler için buffer
    char process_buffer[PROCESS_BUFFER_SIZE] = {0}; // Birikmiş mesajları işlemek için
    int process_buffer_len = 0;

    while (drone->sock > 0) // GÜNCELLENDİ: sock kontrolü
    {
        int len = recv(sock, recv_buffer, sizeof(recv_buffer) - 1, 0);
        if (len <= 0)
        {
            if (len == 0)
                printf("Drone %d: Server connection closed.\n", drone->id);
            else
                perror("recv failed");
            drone->sock = 0; // Diğer thread'lerin durması için
            break;
        }
        recv_buffer[len] = '\0';

        // Gelen veriyi process_buffer'a ekle (memcpy ile daha verimli)
        if (process_buffer_len + len < PROCESS_BUFFER_SIZE)
        {
            memcpy(process_buffer + process_buffer_len, recv_buffer, len);
            process_buffer_len += len;
            process_buffer[process_buffer_len] = '\0'; // Null terminate
        }
        else
        {
            fprintf(stderr, "Drone %d: Processing buffer overflow, discarding data.\n", drone->id);
            process_buffer[0] = '\0'; // Buffer'ı temizle
            process_buffer_len = 0;
            continue;
        }

        char *msg_start = process_buffer;
        char *msg_end;

        while ((msg_end = strchr(msg_start, '\n')) != NULL)
        {
            *msg_end = '\0'; // Mesajı ayır (null-terminate)

            json_object *jobj = json_tokener_parse(msg_start);
            if (!jobj)
            {
                fprintf(stderr, "Drone %d: Failed to parse JSON: %s\n", drone->id, msg_start);
                msg_start = msg_end + 1;
                continue;
            }

            json_object *type_obj = json_object_object_get(jobj, "type");
            const char *type_str = type_obj ? json_object_get_string(type_obj) : NULL;

            if (type_str)
            {
                if (strcmp(type_str, "ASSIGN_MISSION") == 0)
                {
                    pthread_mutex_lock(&drone->lock);
                    json_object *target_json_obj = json_object_object_get(jobj, "target");
                    if (target_json_obj)
                    {
                        drone->target.x = json_object_get_int(json_object_object_get(target_json_obj, "x"));
                        drone->target.y = json_object_get_int(json_object_object_get(target_json_obj, "y"));
                        drone->status = ON_MISSION;
                        printf("Drone %d: Assigned mission to (%d, %d)\n", drone->id, drone->target.x, drone->target.y);
                    }
                    else
                    {
                        fprintf(stderr, "Drone %d: ASSIGN_MISSION message missing 'target'.\n", drone->id);
                    }
                    pthread_mutex_unlock(&drone->lock);
                }
                else if (strcmp(type_str, "HEARTBEAT") == 0) // YENİ: Sunucudan HEARTBEAT alındı
                {
                    // printf("Drone %d: Received HEARTBEAT from server.\n", drone->id);
                    json_object *ack_jobj = json_object_new_object();
                    json_object_object_add(ack_jobj, "type", json_object_new_string("HEARTBEAT_ACK"));
                    char drone_id_ack_str[10];
                    snprintf(drone_id_ack_str, sizeof(drone_id_ack_str), "D%d", drone->id);
                    json_object_object_add(ack_jobj, "drone_id", json_object_new_string(drone_id_ack_str));
                    json_object_object_add(ack_jobj, "timestamp", json_object_new_int64(time(NULL)));
                    send_json(sock, ack_jobj);
                    json_object_put(ack_jobj);
                }
                else if (strcmp(type_str, "HANDSHAKE_ACK") == 0)
                {
                    printf("Drone %d: Handshake ACK received from server.\n", drone->id);
                }
                // Diğer mesaj türleri...
            }
            else
            {
                fprintf(stderr, "Drone %d: Received JSON without 'type' field.\n", drone->id);
            }

            json_object_put(jobj);
            msg_start = msg_end + 1;
        }

        // Kalan (tamamlanmamış) mesajı tamponun başına taşı
        if (msg_start != process_buffer && *msg_start != '\0')
        {
            size_t remaining_len = process_buffer_len - (msg_start - process_buffer);
            memmove(process_buffer, msg_start, remaining_len);
            process_buffer_len = remaining_len;
            process_buffer[process_buffer_len] = '\0'; // Null terminate
        }
        else if (*msg_start == '\0') // Tüm buffer işlendi
        {
            process_buffer[0] = '\0';
            process_buffer_len = 0;
        }
        // else: kalan mesaj zaten buffer'ın başında, bir sonraki recv'de devamı gelecek.
    }

    printf("Drone %d: Main loop exiting. Waiting for threads to join...\n", drone->id);
    // Thread'lerin sonlanmasını bekle (sock = 0 yapıldı)
    pthread_join(navigate_thread, NULL);
    pthread_join(status_thread, NULL);

    if (sock > 0)
        close(sock);
    free_drone(drone);
    printf("Drone %d: Exited.\n", drone->id);
    return 0;
}