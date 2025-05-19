#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <json-c/json.h>
#include "list.h"
#include "drone.h"
#include "survivor.h"

#define PORT 8080
#define MAX_DRONES 50

int compare_survivor(void *a, void *b)
{
    Survivor *s1 = (Survivor *)a;
    Survivor *s2 = (Survivor *)b;
    return s1->id - s2->id;
}

int compare_drone(void *a, void *b)
{
    Drone *d1 = (Drone *)a;
    Drone *d2 = (Drone *)b;
    return d1->id - d2->id;
}

List *drone_list;
List *survivor_list;

void send_json(int sock, json_object *jobj)
{
    const char *str = json_object_to_json_string(jobj);
    send(sock, str, strlen(str), 0);
    send(sock, "\n", 1, 0);
}

void *handle_drone(void *arg)
{
    int sock = *(int *)arg;
    free(arg);
    char buffer[1024];
    Drone *drone = NULL;

    while (1)
    {
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len <= 0)
            break;
        buffer[len] = '\0';

        json_object *jobj = json_tokener_parse(buffer);
        if (!jobj)
            continue;

        const char *type = json_object_get_string(json_object_object_get(jobj, "type"));
        if (strcmp(type, "HANDSHAKE") == 0)
        {
            const char *drone_id = json_object_get_string(json_object_object_get(jobj, "drone_id"));
            // "D" harfini kaldırarak sadece sayıyı al
            char *id_str = (char *)drone_id;
            if (id_str[0] == 'D')
                id_str++;
            int id = atoi(id_str);
            drone = create_drone(id, -1, -1);
            if (!drone)
            {
                printf("Failed to create drone with ID %d\n", id);
                json_object_put(jobj);
                close(sock);
                return NULL;
            }
            drone->sock = sock;
            add_list(drone_list, drone);
            printf("Drone %d connected, added to list\n", id);

            json_object *ack = json_object_new_object();
            json_object_object_add(ack, "type", json_object_new_string("HANDSHAKE_ACK"));
            json_object_object_add(ack, "session_id", json_object_new_string("S123"));
            json_object *config = json_object_new_object();
            json_object_object_add(config, "status_update_interval", json_object_new_int(5));
            json_object_object_add(config, "heartbeat_interval", json_object_new_int(10));
            json_object_object_add(ack, "config", config);
            send_json(sock, ack);
            json_object_put(ack);
        }
        else if (strcmp(type, "STATUS_UPDATE") == 0 && drone)
        {
            pthread_mutex_lock(&drone->lock);
            json_object *loc = json_object_object_get(jobj, "location");
            drone->coord.x = json_object_get_int(json_object_object_get(loc, "x"));
            drone->coord.y = json_object_get_int(json_object_object_get(loc, "y"));
            drone->status = strcmp(json_object_get_string(json_object_object_get(jobj, "status")), "idle") == 0 ? IDLE : ON_MISSION;
            drone->battery = json_object_get_int(json_object_object_get(jobj, "battery"));
            pthread_mutex_unlock(&drone->lock);
            printf("Drone %d status updated: (%d, %d), status: %s, battery: %d\n",
                   drone->id, drone->coord.x, drone->coord.y,
                   drone->status == IDLE ? "IDLE" : "ON_MISSION", drone->battery);
        }
        // server.c - handle_drone fonksiyonu - MISSION_COMPLETE bloğunun düzeltilmiş hali
        // ... (önceki kod) ...
        else if (strcmp(type, "MISSION_COMPLETE") == 0 && drone)
        {
            pthread_mutex_lock(&drone->lock);
            drone->status = IDLE;
            printf("Drone %d completed mission at %ld\n", drone->id, time(NULL));
            pthread_mutex_unlock(&drone->lock);

            pthread_mutex_lock(&survivor_list->lock); // survivor_list kilidini al
            Survivor *found_survivor_data = NULL;
            int min_dist = 999999;
            Node *current_scan_node = survivor_list->head;
            Node *node_to_remove = NULL; // Kaldırılacak olan düğüm (eski closest_node)

            // 1. En yakın survivor'ı ve onu içeren düğümü bul
            while (current_scan_node)
            {
                Survivor *s = (Survivor *)current_scan_node->data;
                if (s)
                { // Verinin NULL olup olmadığını kontrol et (genelde olmaz ama tedbir amaçlı)
                    // Drone koordinatları güncel olmalı. drone->lock bu iterasyon sırasında tutulmuyor.
                    // drone->coord.x/y okunurken değişirse, en "doğru" olanı seçmeyebilir ama çökme yaratmaz.
                    int dist = abs(drone->coord.x - s->coord.x) + abs(drone->coord.y - s->coord.y);
                    if (dist < min_dist)
                    {
                        min_dist = dist;
                        found_survivor_data = s;
                        node_to_remove = current_scan_node;
                    }
                }
                current_scan_node = current_scan_node->next;
            }

            if (node_to_remove) // Eğer bir survivor bulunduysa (dolayısıyla node_to_remove ayarlandıysa)
            {
                // 2. Drone'a yeni görevi ata (found_survivor_data kullanarak)
                pthread_mutex_lock(&drone->lock);
                drone->status = ON_MISSION;
                drone->target = found_survivor_data->coord; // found_survivor_data'dan koordinatları al

                json_object *mission = json_object_new_object();
                json_object_object_add(mission, "type", json_object_new_string("ASSIGN_MISSION"));
                json_object_object_add(mission, "mission_id", json_object_new_string("M123")); // Benzersiz ID kullanmayı düşünün
                json_object_object_add(mission, "priority", json_object_new_string("high"));
                json_object *target_json = json_object_new_object();
                json_object_object_add(target_json, "x", json_object_new_int(found_survivor_data->coord.x));
                json_object_object_add(target_json, "y", json_object_new_int(found_survivor_data->coord.y));
                json_object_object_add(mission, "target", target_json);
                json_object_object_add(mission, "expiry", json_object_new_int(time(NULL) + 3600));
                send_json(drone->sock, mission);
                json_object_put(mission);

                printf("Assigned drone %d to survivor %d at (%d, %d)\n",
                       drone->id, found_survivor_data->id, found_survivor_data->coord.x, found_survivor_data->coord.y);
                pthread_mutex_unlock(&drone->lock);

                // 3. Survivor'ın düğümünü survivor_list'ten kaldır (node_to_remove)
                Node *prev_for_removal = NULL;
                Node *iterator_node = survivor_list->head;

                if (survivor_list->head == node_to_remove)
                { // Eğer kaldırılacak düğüm listenin başıysa
                    survivor_list->head = node_to_remove->next;
                }
                else
                { // Kaldırılacak düğüm listenin başı değilse, önceki düğümü bul
                    while (iterator_node != NULL && iterator_node->next != node_to_remove)
                    {
                        iterator_node = iterator_node->next;
                    }
                    if (iterator_node != NULL)
                    { // Önceki düğüm bulundu (iterator_node şimdi node_to_remove'dan önceki düğüm)
                        prev_for_removal = iterator_node;
                        prev_for_removal->next = node_to_remove->next;
                    }
                    else
                    {
                        // Bu durum normalde olmamalıdır: node_to_remove listede bulundu ama bir önceki düğümü bulunamadı.
                        // Bu, ya node_to_remove'un listenin başı olduğu (yukarıda halledildi) ya da
                        // başka bir thread tarafından liste yapısının kilit olmadan bozulduğu anlamına gelir (burada kilit var, olası değil).
                        fprintf(stderr, "Error: Could not find predecessor for node %p to remove. List may be corrupted.\n", (void *)node_to_remove);
                        // Güvenlik için, bu durumla karşılaşılırsa bellek serbest bırakma işlemi yapılmayabilir.
                        // Şimdilik yapının tutarlı olduğunu varsayıyoruz.
                    }
                }

                // Düğümün listeden düzgün bir şekilde çıkarıldığından emin olduktan sonra belleği serbest bırak
                // (yukarıdaki if/else bloğu bunu sağlamalı)
                survivor_list->size--;
                if (node_to_remove->data)
                {                                        // node_to_remove->data, found_survivor_data ile aynıdır
                    free_survivor(node_to_remove->data); // Survivor verisini serbest bırak
                }
                free(node_to_remove); // Liste düğümünü serbest bırak
            }
            else
            {
                printf("No survivors available for drone %d\n", drone->id);
            }
            pthread_mutex_unlock(&survivor_list->lock); // survivor_list kilidini bırak
        }
        // ... (sonraki kod) ...
        else if (strcmp(type, "BATTERY_DEPLETED") == 0 && drone)
        {
            printf("Drone %d battery depleted, disconnecting\n", drone->id);
            pthread_mutex_lock(&drone_list->lock);
            remove_list(drone_list, drone, compare_drone);
            pthread_mutex_unlock(&drone_list->lock);
            free_drone(drone);
            close(sock);
            json_object_put(jobj);
            return NULL;
        }

        json_object_put(jobj);
    }

    if (drone)
    {
        pthread_mutex_lock(&drone_list->lock);
        remove_list(drone_list, drone, compare_drone);
        pthread_mutex_unlock(&drone_list->lock);
        free_drone(drone);
    }
    close(sock);
    return NULL;
}

void *controller(void *arg)
{
    while (1)
    {
        pthread_mutex_lock(&survivor_list->lock);
        Node *node = survivor_list->head;
        Node *prev = NULL;
        while (node)
        {
            Survivor *s = (Survivor *)node->data;
            Drone *closest = NULL;
            int min_dist = 999999;
            pthread_mutex_lock(&drone_list->lock);
            Node *drone_node = drone_list->head;
            while (drone_node)
            {
                Drone *d = (Drone *)drone_node->data;
                pthread_mutex_lock(&d->lock);
                if (d->status == IDLE)
                {
                    int dist = abs(d->coord.x - s->coord.x) + abs(d->coord.y - s->coord.y);
                    if (dist < min_dist)
                    {
                        min_dist = dist;
                        closest = d;
                    }
                }
                pthread_mutex_unlock(&d->lock);
                drone_node = drone_node->next;
            }
            pthread_mutex_unlock(&drone_list->lock);

            if (closest)
            {
                pthread_mutex_lock(&closest->lock);
                closest->status = ON_MISSION;
                closest->target = s->coord;
                json_object *mission = json_object_new_object();
                json_object_object_add(mission, "type", json_object_new_string("ASSIGN_MISSION"));
                json_object_object_add(mission, "mission_id", json_object_new_string("M123"));
                json_object_object_add(mission, "priority", json_object_new_string("high"));
                json_object *target = json_object_new_object();
                json_object_object_add(target, "x", json_object_new_int(s->coord.x));
                json_object_object_add(target, "y", json_object_new_int(s->coord.y));
                json_object_object_add(mission, "target", target);
                json_object_object_add(mission, "expiry", json_object_new_int(time(NULL) + 3600));
                send_json(closest->sock, mission);
                json_object_put(mission);
                printf("Assigned drone %d to survivor %d at (%d, %d)\n",
                       closest->id, s->id, s->coord.x, s->coord.y);
                pthread_mutex_unlock(&closest->lock);

                // Survivor'ı listeden kaldır
                if (node == survivor_list->head)
                {
                    survivor_list->head = node->next;
                }
                else if (prev)
                {
                    prev->next = node->next;
                }
                survivor_list->size--;
                free_survivor(s);
                free(node);
                node = survivor_list->head; // Listeyi baştan tara
                prev = NULL;
            }
            else
            {
                printf("No idle drones available for survivor %d\n", s->id);
                prev = node;
                node = node->next;
            }
        }
        pthread_mutex_unlock(&survivor_list->lock);
        sleep(1);
    }
    return NULL;
}

void *survivor_generator(void *arg)
{
    static int survivor_id = 0;
    while (1)
    {
        int x = rand() % 100;
        int y = rand() % 100;
        int priority = (rand() % 3) + 1;
        Survivor *s = create_survivor(survivor_id++, x, y, priority);
        add_list(survivor_list, s);
        printf("Generated survivor %d at (%d, %d)\n", s->id, x, y);
        sleep(2);
    }
    return NULL;
}

int main()
{
    srand(time(NULL));
    drone_list = create_list();
    survivor_list = create_list();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT)};
    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, MAX_DRONES);

    pthread_t survivor_thread, controller_thread;
    pthread_create(&survivor_thread, NULL, survivor_generator, NULL);
    pthread_create(&controller_thread, NULL, controller, NULL);
    pthread_detach(survivor_thread);
    pthread_detach(controller_thread);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int *drone_fd = malloc(sizeof(int));
        *drone_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        pthread_t thread;
        pthread_create(&thread, NULL, handle_drone, drone_fd);
        pthread_detach(thread);
    }

    close(server_fd);
    destroy_list(drone_list, free_drone);
    destroy_list(survivor_list, free_survivor);
    return 0;
}