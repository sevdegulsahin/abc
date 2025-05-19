#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <json-c/json.h>
#include "drone.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8080

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
    const char *str = json_object_to_json_string(jobj);
    send(sock, str, strlen(str), 0);
    send(sock, "\n", 1, 0);
}

void *navigate_to_target(void *arg)
{
    Drone *d = (Drone *)arg;
    int sock = d->sock;
    int move_count = 0; // Hareket sayacı

    while (1)
    {
        pthread_mutex_lock(&d->lock);
        if (d->battery <= 0)
        {
            d->status = IDLE;
            printf("Drone %d: Battery depleted, stopping\n", d->id);
            json_object *jobj = json_object_new_object();
            json_object_object_add(jobj, "type", json_object_new_string("BATTERY_DEPLETED"));
            char drone_id[10];
            snprintf(drone_id, sizeof(drone_id), "D%d", d->id);
            json_object_object_add(jobj, "drone_id", json_object_new_string(drone_id));
            json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
            send_json(sock, jobj);
            json_object_put(jobj);
            pthread_mutex_unlock(&d->lock);
            break;
        }

        if (d->status == ON_MISSION)
        {
            int moved = 0; // Hareket olup olmadığını takip et
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
                    d->battery--;   // Her 5 harekette batarya 1 azalır
                    move_count = 0; // Sayaç sıfırlanır
                }
            }

            printf("Drone %d at (%d, %d), moving to (%d, %d), battery: %d, move_count: %d\n",
                   d->id, d->coord.x, d->coord.y, d->target.x, d->target.y, d->battery, move_count);

            if (d->coord.x == d->target.x && d->coord.y == d->target.y)
            {
                d->status = IDLE;
                json_object *jobj = json_object_new_object();
                json_object_object_add(jobj, "type", json_object_new_string("MISSION_COMPLETE"));
                char drone_id[10];
                snprintf(drone_id, sizeof(drone_id), "D%d", d->id);
                json_object_object_add(jobj, "drone_id", json_object_new_string(drone_id));
                json_object_object_add(jobj, "mission_id", json_object_new_string("M123"));
                json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
                json_object_object_add(jobj, "success", json_object_new_boolean(true));
                send_json(sock, jobj);
                json_object_put(jobj);
                printf("Drone %d: Mission completed\n", d->id);
            }
        }
        pthread_mutex_unlock(&d->lock);
        sleep(1);
    }
    return NULL;
}

void *send_status_update(void *arg)
{
    Drone *d = (Drone *)arg;
    int sock = d->sock;
    while (1)
    {
        pthread_mutex_lock(&d->lock);
        json_object *jobj = json_object_new_object();
        json_object_object_add(jobj, "type", json_object_new_string("STATUS_UPDATE"));
        char drone_id[10];
        snprintf(drone_id, sizeof(drone_id), "D%d", d->id);
        json_object_object_add(jobj, "drone_id", json_object_new_string(drone_id));
        json_object_object_add(jobj, "timestamp", json_object_new_int(time(NULL)));
        json_object *loc = json_object_new_object();
        json_object_object_add(loc, "x", json_object_new_int(d->coord.x));
        json_object_object_add(loc, "y", json_object_new_int(d->coord.y));
        json_object_object_add(jobj, "location", loc);
        json_object_object_add(jobj, "status", json_object_new_string(d->status == IDLE ? "idle" : "busy"));
        json_object_object_add(jobj, "battery", json_object_new_int(d->battery));
        json_object_object_add(jobj, "speed", json_object_new_int(5));
        send_json(sock, jobj);
        json_object_put(jobj);
        pthread_mutex_unlock(&d->lock);
        sleep(5);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Kullanım: %s <drone_id>\n", argv[0]);
        return 1;
    }

    int sock = connect_to_server();
    if (sock < 0)
        return 1;

    char *id_str = argv[1];
    if (id_str[0] == 'D')
        id_str++;
    int drone_id = atoi(id_str);
    Drone *drone = create_drone(drone_id, -1, -1);
    drone->sock = sock;

    json_object *handshake = json_object_new_object();
    json_object_object_add(handshake, "type", json_object_new_string("HANDSHAKE"));
    char drone_id_str[10];
    snprintf(drone_id_str, sizeof(drone_id_str), "D%d", drone_id);
    json_object_object_add(handshake, "drone_id", json_object_new_string(drone_id_str));
    json_object *caps = json_object_new_object();
    json_object_object_add(caps, "max_speed", json_object_new_int(30));
    json_object_object_add(caps, "battery_capacity", json_object_new_int(100));
    json_object_object_add(caps, "payload", json_object_new_string("medical"));
    json_object_object_add(handshake, "capabilities", caps);
    send_json(sock, handshake);
    json_object_put(handshake);

    pthread_t navigate_thread, status_thread;
    pthread_create(&navigate_thread, NULL, navigate_to_target, drone);
    pthread_create(&status_thread, NULL, send_status_update, drone);
    pthread_detach(navigate_thread);
    pthread_detach(status_thread);

    char buffer[1024];
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
        if (strcmp(type, "ASSIGN_MISSION") == 0)
        {
            pthread_mutex_lock(&drone->lock);
            json_object *target = json_object_object_get(jobj, "target");
            drone->target.x = json_object_get_int(json_object_object_get(target, "x"));
            drone->target.y = json_object_get_int(json_object_object_get(target, "y"));
            drone->status = ON_MISSION;
            printf("Drone %d assigned mission to (%d, %d)\n", drone->id, drone->target.x, drone->target.y);
            pthread_mutex_unlock(&drone->lock);
        }

        json_object_put(jobj);
    }

    close(sock);
    free_drone(drone);
    return 0;
}