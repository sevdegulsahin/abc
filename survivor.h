#ifndef SURVIVOR_H
#define SURVIVOR_H

#include "drone.h" // Coordinate tanımı için drone.h'yi dahil et

typedef struct
{
    int id;
    Coordinate coord; // Coord yerine Coordinate kullan
    int priority;
} Survivor;

Survivor *create_survivor(int id, int x, int y, int priority);
void free_survivor(void *survivor);

#endif