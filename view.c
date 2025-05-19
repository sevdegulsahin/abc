#include <SDL2/SDL.h>
#include "list.h"
#include "drone.h"
#include "survivor.h"

#define GRID_SIZE 100
#define WINDOW_SIZE 800

void render_list(SDL_Renderer *renderer, List *list, void (*render_func)(SDL_Renderer *, void *))
{
    pthread_mutex_lock(&list->lock);
    Node *node = list->head;
    while (node)
    {
        render_func(renderer, node->data);
        node = node->next;
    }
    pthread_mutex_unlock(&list->lock);
}

void render_drone(SDL_Renderer *renderer, void *data)
{
    Drone *d = (Drone *)data;
    pthread_mutex_lock(&d->lock);
    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255); // Blue for idle drones
    if (d->status == ON_MISSION)
    {
        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Green for mission
        // Draw line to target
        SDL_RenderDrawLine(renderer,
                           d->coord.x * WINDOW_SIZE / GRID_SIZE,
                           d->coord.y * WINDOW_SIZE / GRID_SIZE,
                           d->target.x * WINDOW_SIZE / GRID_SIZE,
                           d->target.y * WINDOW_SIZE / GRID_SIZE);
    }
    SDL_Rect rect = {
        d->coord.x * WINDOW_SIZE / GRID_SIZE - 5,
        d->coord.y * WINDOW_SIZE / GRID_SIZE - 5,
        10, 10};
    SDL_RenderFillRect(renderer, &rect);
    pthread_mutex_unlock(&d->lock);
}

void render_survivor(SDL_Renderer *renderer, void *data)
{
    Survivor *s = (Survivor *)data;
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red for survivors
    SDL_Rect rect = {
        s->coord.x * WINDOW_SIZE / GRID_SIZE - 5,
        s->coord.y * WINDOW_SIZE / GRID_SIZE - 5,
        10, 10};
    SDL_RenderFillRect(renderer, &rect);
}

void visualize(List *drone_list, List *survivor_list)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *window = SDL_CreateWindow("Drone Coordination", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_SIZE, WINDOW_SIZE, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    int running = 1;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                running = 0;
        }

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderClear(renderer);

        render_list(renderer, drone_list, render_drone);
        render_list(renderer, survivor_list, render_survivor);

        SDL_RenderPresent(renderer);
        SDL_Delay(100); // Update 10 times per second
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}