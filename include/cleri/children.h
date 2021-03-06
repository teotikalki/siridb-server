/*
 * children.h - linked list for keeping node results
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 08-03-2016
 *
 */
#pragma once

#include <cleri/node.h>

typedef struct cleri_node_s cleri_node_t;

typedef struct cleri_children_s
{
    cleri_node_t * node;
    struct cleri_children_s * next;
} cleri_children_t;

cleri_children_t * cleri_children_new(void);
void cleri_children_free(cleri_children_t * children);
int cleri_children_add(
        cleri_children_t * children,
        cleri_node_t * node);


