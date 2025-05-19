#include "list.h"
#include <stdlib.h>
#include <string.h>

List *create_list()
{
    List *list = malloc(sizeof(List));
    if (!list)
        return NULL;
    list->head = NULL;
    list->size = 0;
    pthread_mutex_init(&list->lock, NULL);
    pthread_cond_init(&list->not_empty, NULL);
    return list;
}

void destroy_list(List *list, void (*free_data)(void *))
{
    if (!list)
        return;
    pthread_mutex_lock(&list->lock);
    Node *current = list->head;
    while (current)
    {
        Node *next = current->next;
        if (free_data && current->data)
            free_data(current->data);
        free(current);
        current = next;
    }
    list->head = NULL;
    list->size = 0;
    pthread_mutex_unlock(&list->lock);
    pthread_mutex_destroy(&list->lock);
    pthread_cond_destroy(&list->not_empty);
    free(list);
}

bool add_list(List *list, void *data)
{
    Node *node = malloc(sizeof(Node));
    if (!node)
        return false;
    node->data = data;
    node->next = NULL;

    pthread_mutex_lock(&list->lock);
    node->next = list->head;
    list->head = node;
    list->size++;
    pthread_cond_signal(&list->not_empty);
    pthread_mutex_unlock(&list->lock);
    return true;
}

bool remove_list(List *list, void *data, int (*compare)(void *, void *))
{
    pthread_mutex_lock(&list->lock);
    Node *current = list->head;
    Node *prev = NULL;
    while (current)
    {
        if (compare(current->data, data) == 0)
        {
            if (prev)
                prev->next = current->next;
            else
                list->head = current->next;
            free(current);
            list->size--;
            pthread_mutex_unlock(&list->lock);
            return true;
        }
        prev = current;
        current = current->next;
    }
    pthread_mutex_unlock(&list->lock);
    return false;
}

void *pop_list(List *list)
{
    pthread_mutex_lock(&list->lock);
    while (list->head == NULL)
    {
        pthread_cond_wait(&list->not_empty, &list->lock);
    }
    Node *node = list->head;
    void *data = node->data;
    list->head = node->next;
    list->size--;
    free(node);
    pthread_mutex_unlock(&list->lock);
    return data;
}

void iterate_list(List *list, void (*func)(void *))
{
    pthread_mutex_lock(&list->lock);
    Node *current = list->head;
    while (current)
    {
        func(current->data);
        current = current->next;
    }
    pthread_mutex_unlock(&list->lock);
}

int get_size(List *list)
{
    pthread_mutex_lock(&list->lock);
    int size = list->size;
    pthread_mutex_unlock(&list->lock);
    return size;
}