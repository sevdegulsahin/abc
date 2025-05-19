#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> // Added for inet_pton
#include <stdbool.h>   // Added for true
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
    int sock = d->id;
    while (1)
    {
        pthread_mutex_lock(&d->lock);
        if (d->status == ON_MISSION)
        {
            if (d->coord.x < d->target.x)
                d->coord.x++;
            else if (d->coord.x > d->target.x)
                d->coord.x--;
            if (d->coord.y < d->target.y)
                d->coord.y++;
            else if (d->coord.y > d->target.y)
                d->coord.y--;
            d->battery--;

            if (d->coord.x == d->target.x && d->coord.y == d->target.y)
            {
                d->status = IDLE;
                json_object *jobj = json_object_new_object();
                json_object_object_add(jobj, "type", json_object_new_string("MISSION_COMPLETE"));
                json_object_object_add(jobj, "drone_id", json_object_new_string("D1"));
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
    int sock = d->id;
    while (1)
    {
        pthread_mutex_lock(&d->lock);
        json_object *jobj = json_object_new_object();
        json_object_object_add(jobj, "type", json_object_new_string("STATUS_UPDATE"));
        json_object_object_add(jobj, "drone_id", json_object_new_string("D1"));
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

int main()
{
    int sock = connect_to_server();
    if (sock < 0)
        return 1;

    Drone *drone = create_drone(sock, 0, 0);

    json_object *handshake = json_object_new_object();
    json_object_object_add(handshake, "type", json_object_new_string("HANDSHAKE"));
    json_object_object_add(handshake, "drone_id", json_object_new_string("D1"));
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