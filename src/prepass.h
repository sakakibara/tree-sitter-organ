/*
 * prepass.h - line classifier for the tree-sitter-organ scanner.
 *
 * Classifies one line at a time against a bounded container-scope
 * stack (drawers, blocks, latex environments, inlinetasks).  The
 * external scanner drives it line by line and serializes the scope
 * stack into tree-sitter's state buffer; prepass_index.h layers a
 * whole-buffer line index on top for standalone use.
 *
 * Bounds: O(line length) per call, scope depth <= 32, no
 * allocation after prepass_state_new.
 */

#ifndef ORGAN_PREPASS_H
#define ORGAN_PREPASS_H

#include <stddef.h>
#include <stdint.h>

/* Mirrors Emacs `org-inlinetask-min-level` (default 15).  Headlines
 * with this many or more leading stars are inlinetasks; fewer are
 * regular outline headings.  Build-time constant — override via
 * `-DORG_INLINETASK_MIN_LEVEL=N`. */
#ifndef ORG_INLINETASK_MIN_LEVEL
#define ORG_INLINETASK_MIN_LEVEL 15
#endif

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

/* Container scopes tracked while classifying lines.  The innermost
 * scope decides how ambiguous lines (block bodies, drawer contents)
 * are classified. */
typedef enum {
    SCOPE_NONE = 0,
    SCOPE_PROPDRAWER,
    SCOPE_DRAWER,
    SCOPE_GBLOCK,
    SCOPE_LBLOCK,
    SCOPE_DYNBLOCK,
    SCOPE_LATEXENV,
    SCOPE_INLINETASK,
} ScopeKind;

typedef struct prepass_state prepass_state_t;

/* Innermost open scope, or SCOPE_NONE when the stack is empty. */
ScopeKind prepass_scope_top(const prepass_state_t *s);

/* Pop the innermost scope.  The scanner calls this when it terminates
 * an unclosed container itself (a headline interrupting a block) so
 * the scope stack tracks the parser's structure. */
void prepass_scope_pop(prepass_state_t *s);

/* Snapshot/restore for the scope stack.  A single classify call
 * mutates at most one stack slot plus the depth, so {depth, top}
 * restores it exactly.  The scanner snapshots before classifying a
 * line and restores on every path that does not emit a token. */
typedef struct {
    uint8_t depth;
    uint8_t top;
} PrepassScopeSnapshot;

PrepassScopeSnapshot prepass_scope_snapshot(const prepass_state_t *s);
void prepass_scope_restore(prepass_state_t *s, PrepassScopeSnapshot snap);

/* Clear all classification state (the scope stack) without freeing
 * the allocation.  The scanner calls this when tree-sitter hands it
 * a fresh (length == 0) or corrupt serialization buffer. */
void prepass_reset(prepass_state_t *s);

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
 * Serialize the pre-pass state (scope stack) into `buffer`. Returns the
 * number of bytes written, or the required size if `buffer == NULL`
 * (callers may probe size first).
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
