#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <json-c/json.h>
#include "drone.h"
#include "survivor.h"
#include "list.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8081
#define CELL_SIZE 15  // Hücre boyutunu artırdık, daha görünür olsun
#define MAP_WIDTH 60  // Harita genişliği 60
#define MAP_HEIGHT 40 // Harita yüksekliği 40

// SDL global değişkenler
SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Event event;
int window_width, window_height;

// Renk tanımları
const SDL_Color BLACK = {0, 0, 0, 255};
const SDL_Color RED = {255, 0, 0, 255};
const SDL_Color BLUE = {0, 0, 255, 255};
const SDL_Color GREEN = {0, 255, 0, 255};
const SDL_Color WHITE = {255, 255, 255, 255};

// Yerel veri yapıları
List *drone_list;
List *survivor_list;

int connect_to_server()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connect failed");
        return -1;
    }
    return sock;
}

void send_json(int sock, json_object *jobj)
{
    const char *str = json_object_to_json_string(jobj);
    send(sock, str, strlen(str), 0);
    send(sock, "\n", 1, 0);
}

int init_sdl_window()
{
    window_width = MAP_WIDTH * CELL_SIZE;
    window_height = MAP_HEIGHT * CELL_SIZE;

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("Drone Kurtarma Simülasyonu",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              window_width,
                              window_height,
                              SDL_WINDOW_SHOWN);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        SDL_DestroyWindow(window);
        fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    return 0;
}

void draw_cell(int x, int y, SDL_Color color)
{
    // Koordinatların harita sınırları içinde olduğundan emin ol
    if (x < 0 || x >= MAP_HEIGHT || y < 0 || y >= MAP_WIDTH)
        return;

    SDL_Rect rect = {
        .x = y * CELL_SIZE,
        .y = x * CELL_SIZE,
        .w = CELL_SIZE,
        .h = CELL_SIZE};
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

void draw_drones()
{
    pthread_mutex_lock(&drone_list->lock);
    Node *current = drone_list->head;
    while (current)
    {
        Drone *d = (Drone *)current->data;
        pthread_mutex_lock(&d->lock);
        SDL_Color color = (d->status == IDLE) ? BLUE : GREEN;
        draw_cell(d->coord.x, d->coord.y, color);

        if (d->status == ON_MISSION)
        {
            // Hedef koordinatların da harita sınırları içinde olduğundan emin ol
            if (d->target.x >= 0 && d->target.x < MAP_HEIGHT && d->target.y >= 0 && d->target.y < MAP_WIDTH)
            {
                SDL_SetRenderDrawColor(renderer, GREEN.r, GREEN.g, GREEN.b, GREEN.a);
                SDL_RenderDrawLine(
                    renderer,
                    d->coord.y * CELL_SIZE + CELL_SIZE / 2,
                    d->coord.x * CELL_SIZE + CELL_SIZE / 2,
                    d->target.y * CELL_SIZE + CELL_SIZE / 2,
                    d->target.x * CELL_SIZE + CELL_SIZE / 2);
            }
        }
        pthread_mutex_unlock(&d->lock);
        current = current->next;
    }
    pthread_mutex_unlock(&drone_list->lock);
}

void draw_survivors()
{
    pthread_mutex_lock(&survivor_list->lock);
    Node *current = survivor_list->head;
    while (current)
    {
        Survivor *s = (Survivor *)current->data;
        draw_cell(s->coord.x, s->coord.y, RED);
        current = current->next;
    }
    pthread_mutex_unlock(&survivor_list->lock);
}

void draw_grid()
{
    SDL_SetRenderDrawColor(renderer, WHITE.r, WHITE.g, WHITE.b, WHITE.a);
    for (int i = 0; i <= MAP_HEIGHT; i++)
    {
        SDL_RenderDrawLine(renderer, 0, i * CELL_SIZE, window_width, i * CELL_SIZE);
    }
    for (int j = 0; j <= MAP_WIDTH; j++)
    {
        SDL_RenderDrawLine(renderer, j * CELL_SIZE, 0, j * CELL_SIZE, window_height);
    }
}

int draw_map()
{
    SDL_SetRenderDrawColor(renderer, BLACK.r, BLACK.g, BLACK.b, BLACK.a);
    SDL_RenderClear(renderer);

    draw_survivors();
    draw_drones();
    draw_grid();

    SDL_RenderPresent(renderer);
    return 0;
}

int check_events()
{
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            return 1;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
            return 1;
    }
    return 0;
}

void quit_all()
{
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

void update_lists_from_json(json_object *jobj)
{
    const char *type = json_object_get_string(json_object_object_get(jobj, "type"));

    if (strcmp(type, "STATE_UPDATE") == 0)
    {
        destroy_list(drone_list, free_drone);
        destroy_list(survivor_list, free_survivor);
        drone_list = create_list();
        survivor_list = create_list();

        json_object *drones = json_object_object_get(jobj, "drones");
        int drone_count = json_object_array_length(drones);
        for (int i = 0; i < drone_count; i++)
        {
            json_object *d = json_object_array_get_idx(drones, i);
            int id = json_object_get_int(json_object_object_get(d, "id"));
            json_object *loc = json_object_object_get(d, "location");
            int x = json_object_get_int(json_object_object_get(loc, "x"));
            int y = json_object_get_int(json_object_object_get(loc, "y"));
            const char *status = json_object_get_string(json_object_object_get(d, "status"));
            json_object *target = json_object_object_get(d, "target");
            int tx = json_object_get_int(json_object_object_get(target, "x"));
            int ty = json_object_get_int(json_object_object_get(target, "y"));
            int battery = json_object_get_int(json_object_object_get(d, "battery"));

            // Koordinatları 60x40 sınırları içine al
            if (x >= 0 && x < MAP_HEIGHT && y >= 0 && y < MAP_WIDTH)
            {
                Drone *drone = create_drone(id, x, y);
                drone->status = strcmp(status, "idle") == 0 ? IDLE : ON_MISSION;
                drone->target.x = tx;
                drone->target.y = ty;
                drone->battery = battery;
                add_list(drone_list, drone);
            }
        }

        json_object *survivors = json_object_object_get(jobj, "survivors");
        int survivor_count = json_object_array_length(survivors);
        for (int i = 0; i < survivor_count; i++)
        {
            json_object *s = json_object_array_get_idx(survivors, i);
            int id = json_object_get_int(json_object_object_get(s, "id"));
            json_object *loc = json_object_object_get(s, "location");
            int x = json_object_get_int(json_object_object_get(loc, "x"));
            int y = json_object_get_int(json_object_object_get(loc, "y"));
            int priority = json_object_get_int(json_object_object_get(s, "priority"));

            // Koordinatları 60x40 sınırları içine al
            if (x >= 0 && x < MAP_HEIGHT && y >= 0 && y < MAP_WIDTH)
            {
                Survivor *survivor = create_survivor(id, x, y, priority);
                add_list(survivor_list, survivor);
            }
        }
    }
}

int main()
{
    drone_list = create_list();
    survivor_list = create_list();

    int sock = connect_to_server();
    if (sock < 0)
    {
        fprintf(stderr, "Sunucuya bağlanılamadı.\n");
        return 1;
    }

    json_object *handshake = json_object_new_object();
    json_object_object_add(handshake, "type", json_object_new_string("VIEW_HANDSHAKE"));
    send_json(sock, handshake);
    json_object_put(handshake);

    if (init_sdl_window())
    {
        fprintf(stderr, "SDL başlatma başarısız.\n");
        close(sock);
        return 1;
    }

    char buffer[4096];
    while (1)
    {
        if (check_events())
        {
            quit_all();
            break;
        }

        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len <= 0)
        {
            fprintf(stderr, "Sunucu bağlantısı kesildi.\n");
            break;
        }
        buffer[len] = '\0';

        json_object *jobj = json_tokener_parse(buffer);
        if (jobj)
        {
            update_lists_from_json(jobj);
            json_object_put(jobj);
        }

        draw_map();
        SDL_Delay(100);
    }

    close(sock);
    destroy_list(drone_list, free_drone);
    destroy_list(survivor_list, free_survivor);
    quit_all();
    return 0;
}