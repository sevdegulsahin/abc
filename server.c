#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> // Added for socket functions
#include <json-c/json.h>
#include "list.h"
#include "drone.h"
#include "survivor.h"

#define PORT 8080
#define MAX_DRONES 50

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
            drone = create_drone(atoi(drone_id + 1), 0, 0);
            add_list(drone_list, drone);

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
        }
        else if (strcmp(type, "MISSION_COMPLETE") == 0 && drone)
        {
            pthread_mutex_lock(&drone->lock);
            drone->status = IDLE;
            printf("Drone %d completed mission\n", drone->id);
            pthread_mutex_unlock(&drone->lock);
        }

        json_object_put(jobj);
    }

    if (drone)
        remove_list(drone_list, drone, (int (*)(void *, void *))strcmp);
    close(sock);
    return NULL;
}

void *controller(void *arg)
{
    while (1)
    {
        pthread_mutex_lock(&survivor_list->lock);
        if (survivor_list->head)
        {
            Survivor *s = (Survivor *)survivor_list->head->data;
            Drone *closest = NULL;
            int min_dist = 999999;
            pthread_mutex_lock(&drone_list->lock);
            Node *node = drone_list->head;
            while (node)
            {
                Drone *d = (Drone *)node->data;
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
                node = node->next;
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
                send_json(closest->id, mission);
                json_object_put(mission);
                printf("Assigned drone %d to survivor %d\n", closest->id, s->id);
                pthread_mutex_unlock(&closest->lock);
                pop_list(survivor_list);
                free_survivor(s);
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