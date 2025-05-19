#include "drone.h"
#include <stdlib.h>

Drone *create_drone(int id, int x, int y)
{
    Drone *drone = malloc(sizeof(Drone));
    if (!drone)
        return NULL;
    drone->id = id;
    drone->status = IDLE;
    drone->coord.x = x;
    drone->coord.y = y;
    drone->target.x = x;
    drone->target.y = y;
    drone->battery = 100;
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