#include "prepass_index.h"
#include "interval_tree.h"

#include <stdlib.h>
#include <string.h>

struct prepass_index {
    prepass_state_t  *state;
    interval_tree_t  *tree;
};

prepass_index_t *prepass_index_new(void) {
    prepass_index_t *ix =
        (prepass_index_t *)calloc(1, sizeof(prepass_index_t));
    if (!ix) return NULL;
    ix->state = prepass_state_new();
    ix->tree = interval_tree_new();
    if (!ix->state || !ix->tree) {
        prepass_index_free(ix);
        return NULL;
    }
    return ix;
}

void prepass_index_free(prepass_index_t *ix) {
    if (!ix) return;
    prepass_state_free(ix->state);
    interval_tree_free(ix->tree);
    free(ix);
}

size_t prepass_index_scan(prepass_index_t *ix,
                          const uint8_t *src, size_t len,
                          LineToken *out, size_t out_capacity) {
    prepass_reset(ix->state);
    interval_tree_splice(ix->tree, 0, interval_tree_size(ix->tree), NULL, 0);
    size_t pos = 0;

    while (pos < len) {
        size_t line_start = pos;
        const uint8_t *nl = memchr(src + pos, '\n', len - pos);
        size_t line_end = nl ? (size_t)(nl - src) : len;
        pos = line_end;
        uint32_t line_len = (uint32_t)(line_end - line_start);

        const uint8_t *line = src + line_start;
        LineClassification c =
            prepass_classify_line(ix->state, line, line_len);

        LineToken tok = {
            .type = c.type,
            .start_byte = (uint32_t)line_start,
            .end_byte = (uint32_t)line_end,
            .indent_col = c.indent_col,
            .stack_depth_before = c.stack_depth_before,
            ._pad = 0,
            .meta = c.meta,
        };
        interval_tree_push(ix->tree, &tok);

        if (pos < len) pos++;
    }

    size_t total = interval_tree_size(ix->tree);
    if (out && out_capacity > 0) {
        size_t n = total < out_capacity ? total : out_capacity;
        for (size_t i = 0; i < n; i++) {
            out[i] = *interval_tree_at(ix->tree, i);
        }
    }
    return total;
}

EditRange prepass_index_locate_edit(prepass_index_t *ix,
                                    uint32_t start_byte,
                                    uint32_t old_end_byte) {
    size_t total = interval_tree_size(ix->tree);
    EditRange r;
    r.start_token = interval_tree_index_for_byte(ix->tree, start_byte);
    r.end_token   = interval_tree_index_for_byte(ix->tree, old_end_byte);
    if (r.end_token < total) {
        const LineToken *tok = interval_tree_at(ix->tree, r.end_token);
        if (tok && tok->start_byte < old_end_byte) {
            r.end_token++;
        }
    }
    if (r.end_token > total) r.end_token = total;
    const LineToken *first = interval_tree_at(ix->tree, r.start_token);
    r.restart_byte = first ? first->start_byte : 0;
    return r;
}

size_t prepass_index_apply_edit(prepass_index_t *ix,
                                const uint8_t *new_src, size_t new_len,
                                uint32_t start_byte,
                                uint32_t old_end_byte,
                                uint32_t new_end_byte,
                                LineToken *out, size_t out_capacity) {
    size_t total_old = interval_tree_size(ix->tree);

    if (total_old == 0) {
        return prepass_index_scan(ix, new_src, new_len, out, out_capacity);
    }

    EditRange r = prepass_index_locate_edit(ix, start_byte, old_end_byte);
    if (r.start_token >= total_old) {
        return prepass_index_scan(ix, new_src, new_len, out, out_capacity);
    }

    const LineToken *start_tok = interval_tree_at(ix->tree, r.start_token);
    if (start_tok->stack_depth_before > 0) {
        return prepass_index_scan(ix, new_src, new_len, out, out_capacity);
    }

    int32_t delta = (int32_t)new_end_byte - (int32_t)old_end_byte;
    size_t pos = start_tok->start_byte;
    prepass_reset(ix->state);

    LineToken *new_buf = (LineToken *)malloc(16 * sizeof(LineToken));
    if (!new_buf) abort();
    size_t new_count = 0;
    size_t new_cap = 16;

    size_t old_idx = r.start_token;
    int converged = 0;

    while (pos < new_len && !converged) {
        size_t line_start = pos;
        const uint8_t *nl = memchr(new_src + pos, '\n', new_len - pos);
        size_t line_end = nl ? (size_t)(nl - new_src) : new_len;
        pos = line_end;
        uint32_t line_len = (uint32_t)(line_end - line_start);

        const uint8_t *line = new_src + line_start;
        LineClassification c =
            prepass_classify_line(ix->state, line, line_len);

        if (new_count >= new_cap) {
            new_cap *= 2;
            new_buf = (LineToken *)realloc(new_buf, new_cap * sizeof(LineToken));
            if (!new_buf) abort();
        }
        new_buf[new_count++] = (LineToken){
            .type = c.type,
            .start_byte = (uint32_t)line_start,
            .end_byte = (uint32_t)line_end,
            .indent_col = c.indent_col,
            .stack_depth_before = c.stack_depth_before,
            ._pad = 0,
            .meta = c.meta,
        };

        if (pos < new_len) pos++;

        if (line_start >= (size_t)new_end_byte && old_idx < total_old) {
            const LineToken *old_tok = interval_tree_at(ix->tree, old_idx);
            int32_t expected_old_start = (int32_t)line_start - delta;
            if (expected_old_start == (int32_t)old_tok->start_byte
                    && c.type == old_tok->type
                    && c.stack_depth_before == old_tok->stack_depth_before) {
                new_count--;
                converged = 1;
            } else {
                old_idx++;
            }
        }
    }

    if (!converged) {
        old_idx = total_old;
    }

    interval_tree_splice(ix->tree, r.start_token,
                         old_idx - r.start_token, new_buf, new_count);
    free(new_buf);

    if (converged && delta != 0) {
        size_t after = r.start_token + new_count;
        size_t tail_total = interval_tree_size(ix->tree);
        for (size_t i = after; i < tail_total; i++) {
            LineToken *tok = (LineToken *)interval_tree_at(ix->tree, i);
            tok->start_byte = (uint32_t)((int32_t)tok->start_byte + delta);
            tok->end_byte   = (uint32_t)((int32_t)tok->end_byte   + delta);
        }
    }

    size_t total = interval_tree_size(ix->tree);
    if (out && out_capacity > 0) {
        size_t n = total < out_capacity ? total : out_capacity;
        for (size_t i = 0; i < n; i++) {
            out[i] = *interval_tree_at(ix->tree, i);
        }
    }
    return total;
}
