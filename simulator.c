#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "list.h"
#include "drone.h"
#include "survivor.h"

List *drone_list;
List *survivor_list;

void *drone_behavior(void *arg)
{
    Drone *d = (Drone *)arg;
    while (1)
    {
        pthread_mutex_lock(&d->lock);
        if (d->status == ON_MISSION)
        {
            // Move toward target (1 cell per iteration)
            if (d->coord.x < d->target.x)
                d->coord.x++;
            else if (d->coord.x > d->target.x)
                d->coord.x--;
            if (d->coord.y < d->target.y)
                d->coord.y++;
            else if (d->coord.y > d->target.y)
                d->coord.y--;
            d->battery--;

            // Check mission completion
            if (d->coord.x == d->target.x && d->coord.y == d->target.y)
            {
                d->status = IDLE;
                printf("Drone %d: Mission completed!\n", d->id);
            }
        }
        pthread_mutex_unlock(&d->lock);
        sleep(1); // Update every second
    }
    return NULL;
}

void *survivor_generator(void *arg)
{
    static int survivor_id = 0;
    while (1)
    {
        int x = rand() % 100; // Grid size: 100x100
        int y = rand() % 100;
        int priority = (rand() % 3) + 1; // 1 to 3
        Survivor *s = create_survivor(survivor_id++, x, y, priority);
        add_list(survivor_list, s);
        printf("Generated survivor %d at (%d, %d)\n", s->id, x, y);
        sleep(2);
    }
    return NULL;
}

void *controller(void *arg)
{
    while (1)
    {
        // Assign missions: closest idle drone to oldest survivor
        pthread_mutex_lock(&survivor_list->lock);
        if (survivor_list->head)
        {
            Survivor *s = (Survivor *)survivor_list->head->data;
            Drone *closest = NULL;
            int min_dist = 999999;
            // Iterate drones to find closest idle one
            pthread_mutex_lock(&drone_list->lock);
            Node *node = drone_list->head;
            while (node)
            {
                Drone *d = (Drone *)node->data;
                pthread_mutex_lock(&d->lock);
                if (d->status == IDLE)
                {
                    int dist = abs(d->coord.x - s->coord.x) + abs(d->coord.y - s->coord.y);
                    if (dist < min_dist)
                    {
                        min_dist = dist;
                        closest = d;
                    }
                }
                pthread_mutex_unlock(&d->lock);
                node = node->next;
            }
            pthread_mutex_unlock(&drone_list->lock);

            if (closest)
            {
                pthread_mutex_lock(&closest->lock);
                closest->status = ON_MISSION;
                closest->target = s->coord;
                printf("Assigned drone %d to survivor %d\n", closest->id, s->id);
                pthread_mutex_unlock(&closest->lock);
                pop_list(survivor_list); // Remove survivor
                free_survivor(s);
            }
        }
        pthread_mutex_unlock(&survivor_list->lock);
        sleep(1);
    }
    return NULL;
}

int main()
{
    srand(time(NULL));
    drone_list = create_list();
    survivor_list = create_list();

    // Create 5 drones
    for (int i = 0; i < 5; i++)
    {
        Drone *d = create_drone(i, rand() % 100, rand() % 100);
        add_list(drone_list, d);
        pthread_t thread;
        pthread_create(&thread, NULL, drone_behavior, d);
        pthread_detach(thread);
    }

    // Start survivor generator
    pthread_t survivor_thread;
    pthread_create(&survivor_thread, NULL, survivor_generator, NULL);
    pthread_detach(survivor_thread);

    // Start controller
    pthread_t controller_thread;
    pthread_create(&controller_thread, NULL, controller, NULL);
    pthread_detach(controller_thread);

    // Main loop (for SDL visualization, implemented separately)
    while (1)
    {
        sleep(10); // Keep main thread alive
    }

    destroy_list(drone_list, free_drone);
    destroy_list(survivor_list, free_survivor);
    return 0;
}