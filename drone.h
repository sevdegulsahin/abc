#ifndef DRONE_H
#define DRONE_H

#include <pthread.h>

typedef enum
{
    IDLE,
    ON_MISSION,
    CHARGING
} DroneStatus;

typedef struct
{
    int x, y;
} Coord;

typedef struct
{
    int id;
    DroneStatus status;
    Coord coord;
    Coord target;
    int battery;
    pthread_mutex_t lock; // Drone-specific lock
} Drone;

Drone *create_drone(int id, int x, int y);
void free_drone(void *drone);

#endif