#ifndef LIST_H
#define LIST_H

#include <pthread.h>
#include <stdbool.h>

typedef struct Node
{
    void *data;
    struct Node *next;
} Node;

typedef struct List
{
    Node *head;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    int size;
} List;

List *create_list();
void destroy_list(List *list, void (*free_data)(void *));
bool add_list(List *list, void *data);
bool remove_list(List *list, void *data, int (*compare)(void *, void *));
void *pop_list(List *list);
void iterate_list(List *list, void (*func)(void *));
int get_size(List *list);

#endif