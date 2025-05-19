#ifndef DRONE_H
#define DRONE_H

#include <pthread.h>

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
    int sock; // Add socket descriptor
    pthread_mutex_t lock;
} Drone;

Drone *create_drone(int id, int x, int y);
void free_drone(void *drone);

#endif