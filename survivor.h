// survivor.h
#ifndef SURVIVOR_H
#define SURVIVOR_H

#include "drone.h"   // Coordinate tanımı için
#include <stdbool.h> // bool için

typedef struct
{
    int id;
    Coordinate coord;
    int priority;
    bool is_targeted; // YENİ: Bu survivor'a bir drone yönlendirildi mi?
} Survivor;

Survivor *create_survivor(int id, int x, int y, int priority);
void free_survivor(void *survivor);

#endif