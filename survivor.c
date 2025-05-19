// survivor.c
#include "survivor.h"
#include <stdlib.h>

Survivor *create_survivor(int id, int x, int y, int priority)
{
    Survivor *survivor = malloc(sizeof(Survivor));
    if (!survivor)
        return NULL;
    survivor->id = id;
    survivor->coord.x = x;
    survivor->coord.y = y;
    survivor->priority = priority;
    survivor->is_targeted = false; // Başlangıçta false
    return survivor;
}

void free_survivor(void *survivor)
{
    free(survivor);
}