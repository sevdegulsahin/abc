#ifndef DRONE_H
#define DRONE_H

#include <pthread.h>
#include <time.h> // YENİ: time_t için

typedef enum
{
    IDLE,
    ON_MISSION
} DroneStatus;

typedef struct
{
    int x;
    int y;
} Coordinate;

typedef struct
{
    int id;
    DroneStatus status;
    Coordinate coord;
    Coordinate target;
    int battery;
    int sock;
    pthread_mutex_t lock;
    time_t last_message_time; // YENİ: Drone'dan gelen son mesaj zamanı (status veya ack)
} Drone;

Drone *create_drone(int id, int x, int y);
void free_drone(void *drone);

#endif