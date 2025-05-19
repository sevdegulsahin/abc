#include "drone.h"
#include <stdlib.h>
#include <time.h>

static int initialized = 0;
Drone *create_drone(int id, int x, int y)
{
    if (!initialized)
    {
        srand(time(NULL));
        initialized = 1;
    }
    Drone *drone = malloc(sizeof(Drone));
    if (!drone)
        return NULL;
    drone->id = id;
    drone->status = IDLE;
    drone->coord.x = (x == -1) ? (rand() % 40) : x; // 0-39
    drone->coord.y = (y == -1) ? (rand() % 60) : y; // 0-59
    drone->target.x = drone->coord.x;
    drone->target.y = drone->coord.y;
    drone->battery = 100;
    drone->sock = 0;
    pthread_mutex_init(&drone->lock, NULL);
    return drone;
}

void free_drone(void *drone)
{
    if (!drone)
        return;
    Drone *d = (Drone *)drone;
    pthread_mutex_destroy(&d->lock);
    free(d);
}