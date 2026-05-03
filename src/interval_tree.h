#ifndef ORGAN_INTERVAL_TREE_H
#define ORGAN_INTERVAL_TREE_H

#include <stddef.h>
#include <stdint.h>
#include "prepass.h"

typedef struct interval_tree interval_tree_t;

interval_tree_t *interval_tree_new(void);
void             interval_tree_free(interval_tree_t *t);

size_t   interval_tree_size(const interval_tree_t *t);
uint32_t interval_tree_depth(const interval_tree_t *t);

void interval_tree_push(interval_tree_t *t, const LineToken *tok);
const LineToken *interval_tree_at(const interval_tree_t *t, size_t i);

void interval_tree_splice(interval_tree_t *t,
                          size_t start, size_t old_count,
                          const LineToken *new_tokens, size_t new_count);

size_t interval_tree_index_for_byte(const interval_tree_t *t, uint32_t byte_offset);

#endif /* ORGAN_INTERVAL_TREE_H */
