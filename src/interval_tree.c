#include "interval_tree.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 64

struct interval_tree {
    LineToken *items;
    size_t     count;
    size_t     cap;
};

interval_tree_t *interval_tree_new(void) {
    interval_tree_t *t = (interval_tree_t *)calloc(1, sizeof(interval_tree_t));
    if (!t) return NULL;
    t->items = (LineToken *)calloc(INITIAL_CAP, sizeof(LineToken));
    t->cap = INITIAL_CAP;
    return t;
}

void interval_tree_free(interval_tree_t *t) {
    if (!t) return;
    free(t->items);
    free(t);
}

size_t interval_tree_size(const interval_tree_t *t) {
    return t ? t->count : 0;
}

uint32_t interval_tree_depth(const interval_tree_t *t) {
    return t && t->count ? 1 : 0;
}

static void grow(interval_tree_t *t, size_t needed) {
    if (t->cap >= needed) return;
    size_t new_cap = t->cap;
    while (new_cap < needed) new_cap *= 2;
    LineToken *new_items = (LineToken *)realloc(t->items, new_cap * sizeof(LineToken));
    if (!new_items) abort();
    t->items = new_items;
    t->cap = new_cap;
}

void interval_tree_push(interval_tree_t *t, const LineToken *tok) {
    grow(t, t->count + 1);
    t->items[t->count++] = *tok;
}

const LineToken *interval_tree_at(const interval_tree_t *t, size_t i) {
    if (!t || i >= t->count) return NULL;
    return &t->items[i];
}

void interval_tree_splice(interval_tree_t *t,
                          size_t start, size_t old_count,
                          const LineToken *new_tokens, size_t new_count) {
    if (!t || start > t->count || start + old_count > t->count) return;
    size_t needed = t->count - old_count + new_count;
    grow(t, needed);
    if (new_count != old_count) {
        memmove(&t->items[start + new_count],
                &t->items[start + old_count],
                (t->count - start - old_count) * sizeof(LineToken));
    }
    if (new_count > 0) {
        memcpy(&t->items[start], new_tokens, new_count * sizeof(LineToken));
    }
    t->count = needed;
}

size_t interval_tree_index_for_byte(const interval_tree_t *t, uint32_t byte_offset) {
    if (!t || t->count == 0) return 0;
    size_t lo = 0, hi = t->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->items[mid].end_byte <= byte_offset) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
