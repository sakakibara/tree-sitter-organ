/*
 * prepass.h — Layer 1 line tokenizer.
 *
 * Phases (all complete):
 *   3. Scalar pre-pass: single linear scan, one LineToken per line.
 *   4. Interval-tree storage: O(log N) lookup, O(log N + delta) splice.
 *   5. Incremental updates via prepass_apply_edit + serialize/deserialize.
 *   6. SIMD acceleration of hot paths (whitespace skip via SWAR; line
 *      detection via libc memchr). Scalar fallback selectable via
 *      `ORGAN_PREPASS_USE_SIMD=0` build flag.
 *
 * Bounded resources:
 *   - Time:   O(input_length) on full scan; O(edit_size + boundary_fixup)
 *             on incremental updates with depth-0 restart point.
 *   - Memory: O(line_count); internal stack depth ≤ 32.
 *   - No backtracking, no recursive recognizers, no per-call allocation.
 *
 * Plan B deliverable: a tested standalone C library. Plan C will wire it
 * into the tree-sitter-organ block-grammar scanner.
 */

#ifndef ORGAN_PREPASS_H
#define ORGAN_PREPASS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TT_BODY = 0,
    TT_EMPTY,
    TT_HEADING,
    TT_PLANNING,
    TT_PROPDRAWER_OPEN,
    TT_PROPDRAWER_CLOSE,
    TT_NODE_PROPERTY,
    TT_DRAWER_OPEN,
    TT_DRAWER_CLOSE,
    TT_GBLOCK_OPEN,
    TT_GBLOCK_CLOSE,
    TT_LBLOCK_OPEN,
    TT_LBLOCK_BODY,
    TT_LBLOCK_CLOSE,
    TT_DYNBLOCK_OPEN,
    TT_DYNBLOCK_CLOSE,
    TT_LATEXENV_OPEN,
    TT_LATEXENV_BODY,
    TT_LATEXENV_CLOSE,
    TT_KEYWORD,
    TT_AFFILIATED_KEYWORD,
    TT_COMMENT,
    TT_FIXED_WIDTH,
    TT_HRULE,
    TT_TABLE_ROW,
    TT_TABLE_RULE,
    TT_LIST_ITEM,
    TT_FOOTNOTE_DEF,
    TT_INLINETASK_OPEN,
    TT_INLINETASK_CLOSE,
    TT_CLOCK,
    TT_DIARY_SEXP,
    LINE_TOKEN_TYPE_COUNT
} LineTokenType;

typedef struct {
    LineTokenType type;
    uint32_t      start_byte;
    uint32_t      end_byte;
    uint16_t      indent_col;
    uint8_t       stack_depth_before;
    uint8_t       _pad;
    uint64_t      meta;
} LineToken;

typedef struct {
    LineTokenType type;
    uint16_t      indent_col;
    uint8_t       stack_depth_before;
    uint64_t      meta;
} LineClassification;

typedef struct prepass_state prepass_state_t;

prepass_state_t *prepass_state_new(void);
void             prepass_state_free(prepass_state_t *s);

/*
 * Classify a single line. Updates the scope stack as a side effect.
 * Used by the tree-sitter external scanner to emit one token per line
 * during incremental parsing. The caller fills in start_byte/end_byte
 * for the LineToken being constructed (not returned by this function
 * since the byte offsets are caller-tracked).
 */
LineClassification prepass_classify_line(prepass_state_t *s,
                                          const uint8_t *line,
                                          uint32_t line_len);

/*
 * Scan src[0..len) into LineTokens. Writes up to out_capacity tokens to out.
 * Returns the total number of tokens that WOULD be produced (>= out_capacity
 * is allowed; caller reallocates and retries).
 */
size_t prepass_scan(prepass_state_t *s,
                    const uint8_t *src, size_t len,
                    LineToken *out, size_t out_capacity);

typedef struct {
    size_t   start_token;
    size_t   end_token;       /* exclusive */
    uint32_t restart_byte;
} EditRange;

EditRange prepass_locate_edit(prepass_state_t *s,
                              uint32_t start_byte,
                              uint32_t old_end_byte);

/*
 * Apply an edit and re-tokenize the affected range incrementally.
 *
 * `new_src` / `new_len` is the buffer AFTER the edit.
 * `start_byte` is where the edit begins (in both old and new buffers).
 * `old_end_byte` is the end of the edit's range in the OLD buffer.
 * `new_end_byte` is the end of the edit's range in the NEW buffer.
 *
 * For a pure insert at offset N: start_byte = old_end_byte = N,
 *                                new_end_byte = N + insert_size.
 * For a pure delete at [N, M]:    start_byte = N, old_end_byte = M,
 *                                new_end_byte = N.
 * For a replace at [N, M] -> M':  old_end_byte = M, new_end_byte = M'.
 *
 * Returns the total token count after the edit (caller-allocates `out`).
 */
size_t prepass_apply_edit(prepass_state_t *s,
                          const uint8_t *new_src, size_t new_len,
                          uint32_t start_byte,
                          uint32_t old_end_byte,
                          uint32_t new_end_byte,
                          LineToken *out, size_t out_capacity);

/*
 * Serialize the entire pre-pass state (scope stack + interval tree) into
 * `buffer`. Returns the number of bytes written, or the required size if
 * `buffer == NULL` (callers may probe size first).
 */
size_t prepass_serialize(const prepass_state_t *s,
                         uint8_t *buffer, size_t buffer_capacity);

/*
 * Restore state from `buffer`. Returns 1 on success, 0 if `buffer_size`
 * is incompatible. Existing state is replaced.
 */
int prepass_deserialize(prepass_state_t *s,
                        const uint8_t *buffer, size_t buffer_size);

#endif /* ORGAN_PREPASS_H */
