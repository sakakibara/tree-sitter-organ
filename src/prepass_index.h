/* prepass_index.h - standalone whole-buffer line index built on the
 * prepass classifier: full scans, incremental edits, and interval
 * storage of LineTokens.  Not linked into the tree-sitter scanner
 * (org.so); used by the prepass.so library builds. */

#ifndef ORGAN_PREPASS_INDEX_H
#define ORGAN_PREPASS_INDEX_H

#include "prepass.h"

typedef struct prepass_index prepass_index_t;

prepass_index_t *prepass_index_new(void);
void             prepass_index_free(prepass_index_t *ix);

/*
 * Scan src[0..len) into LineTokens.  Writes up to out_capacity
 * tokens to out; returns the total number that WOULD be produced
 * (caller reallocates and retries when larger).
 */
size_t prepass_index_scan(prepass_index_t *ix,
                          const uint8_t *src, size_t len,
                          LineToken *out, size_t out_capacity);

typedef struct {
    size_t   start_token;
    size_t   end_token;       /* exclusive */
    uint32_t restart_byte;
} EditRange;

EditRange prepass_index_locate_edit(prepass_index_t *ix,
                                    uint32_t start_byte,
                                    uint32_t old_end_byte);

/*
 * Apply an edit and re-tokenize incrementally.  new_src/new_len is
 * the buffer AFTER the edit; start_byte is where the edit begins in
 * both buffers; old_end_byte / new_end_byte are the edit range ends
 * in the old / new buffer.  Returns the total token count after.
 */
size_t prepass_index_apply_edit(prepass_index_t *ix,
                                const uint8_t *new_src, size_t new_len,
                                uint32_t start_byte,
                                uint32_t old_end_byte,
                                uint32_t new_end_byte,
                                LineToken *out, size_t out_capacity);

#endif /* ORGAN_PREPASS_INDEX_H */
