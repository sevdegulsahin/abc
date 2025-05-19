// survivor.h
#ifndef SURVIVOR_H
#define SURVIVOR_H

#include "drone.h"   // Coordinate tanımı için
#include <stdbool.h> // bool için
#include <time.h>    // YENİ: time_t için

typedef struct
{
    int id;
    Coordinate coord;
    int priority;
    bool is_targeted;
    time_t creation_time; // YENİ: Survivor'ın oluşturulma zamanı
} Survivor;

Survivor *create_survivor(int id, int x, int y, int priority);
void free_survivor(void *survivor);

#endif