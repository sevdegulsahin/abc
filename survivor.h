#ifndef SURVIVOR_H
#define SURVIVOR_H

#include "drone.h" // Include drone.h for Coord definition

typedef struct
{
    int id;
    Coord coord;
    int priority;
} Survivor;

Survivor *create_survivor(int id, int x, int y, int priority);
void free_survivor(void *survivor);

#endif