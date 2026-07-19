#include "tree_sitter/parser.h"
#include "prepass.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inlinetask threshold.  Mirrors Emacs `org-inlinetask-min-level`
 * (default 15): a headline with N stars where N >= this value is
 * parsed as an inlinetask, not a regular outline heading.  Tree-sitter
 * scanners can't read user config at parse time, so this is a
 * build-time constant.  Override by passing `-DORG_INLINETASK_MIN_LEVEL=N`
 * on the cc line and rebuilding (`make build CFLAGS+=-DORG_INLINETASK_MIN_LEVEL=16`). */
#ifndef ORG_INLINETASK_MIN_LEVEL
#define ORG_INLINETASK_MIN_LEVEL 15
#endif

/* External-token symbols. ORDER MUST MATCH grammar.js's `externals: $ => [...]`
 * declaration EXACTLY. */
enum OrgExternal {
    EXT_HEADING_OPEN = 0,
    EXT_HEADING_CLOSE,
    EXT_HEADLINE_TODO,
    EXT_HEADLINE_COMMENT,        /* literal "COMMENT" keyword */
    EXT_HEADLINE_PRIORITY,
    EXT_HEADLINE_TITLE,
    EXT_HEADLINE_STATS_COOKIE,   /* `[N%]` or `[N/M]` at end of title */
    EXT_HEADLINE_TAG_LIST_OPEN,  /* zero-width validator at tag-list start */
    EXT_LIST_CHECKBOX,           /* `[ ]` / `[x]` / `[X]` / `[-]` after bullet */
    EXT_PLANNING_LINE,
    EXT_PROPDRAWER_OPEN,
    EXT_PROPDRAWER_CLOSE,
    EXT_NODE_PROPERTY_LINE,
    EXT_DRAWER_OPEN,
    EXT_DRAWER_CLOSE,
    EXT_GBLOCK_OPEN,
    EXT_GBLOCK_CLOSE,
    EXT_SRC_BLOCK_OPEN,
    EXT_SRC_BLOCK_CLOSE,
    EXT_EXAMPLE_BLOCK_OPEN,
    EXT_EXAMPLE_BLOCK_CLOSE,
    EXT_EXPORT_BLOCK_OPEN,
    EXT_EXPORT_BLOCK_CLOSE,
    EXT_VERSE_BLOCK_OPEN,
    EXT_VERSE_BLOCK_CLOSE,
    EXT_COMMENT_BLOCK_OPEN,
    EXT_COMMENT_BLOCK_CLOSE,
    EXT_LBLOCK_BODY,
    EXT_DYNBLOCK_OPEN,
    EXT_DYNBLOCK_CLOSE,
    EXT_LATEXENV_OPEN,
    EXT_LATEXENV_BODY,
    EXT_LATEXENV_CLOSE,
    EXT_KEYWORD_LINE,
    EXT_AFFILIATED_KEYWORD_LINE,
    EXT_COMMENT_LINE,
    EXT_FIXED_WIDTH_LINE,
    EXT_HRULE_LINE,
    EXT_TABLE_ROW_START,
    EXT_TABLE_PIPE,
    EXT_TABLE_CELL_CONTENT,
    EXT_TABLE_ROW_END,
    EXT_TABLE_RULE_LINE,
    EXT_LIST_ITEM_BULLET,
    EXT_PLAIN_LIST_OPEN,
    EXT_PLAIN_LIST_CLOSE,
    EXT_FOOTNOTE_DEF_LINE,
    EXT_INLINETASK_OPEN,
    EXT_INLINETASK_CLOSE,
    EXT_CLOCK_LINE,
    EXT_DIARY_SEXP_LINE,
    EXT_INLINE_CONTENT_LINE,
    EXT_EMPTY_LINE,
    EXT_COMMENT_BODY_TEXT,
    EXT_FIXED_WIDTH_BODY_TEXT,
    EXT_LIST_COUNTER,            /* `[@N]` force-renumber cookie after a bullet */
    EXT_ITEM_TAG_TEXT,           /* description-list term before ` :: ` */
    EXT_ITEM_TAG_SEP,            /* the ` :: ` separator after an item tag */
    EXT_FN_EMPTY_LINE,           /* empty line inside a footnote definition body */
    EXT_DIARY_SEXP_BODY,         /* diary sexp body up to the line's last `)` */
};

/* Map prepass LineTokenType → tree-sitter external symbol (non-heading types). */
static int prepass_to_external(LineTokenType t) {
    switch (t) {
        case TT_PLANNING:         return EXT_PLANNING_LINE;
        case TT_PROPDRAWER_OPEN:  return EXT_PROPDRAWER_OPEN;
        case TT_PROPDRAWER_CLOSE: return EXT_PROPDRAWER_CLOSE;
        case TT_NODE_PROPERTY:    return EXT_NODE_PROPERTY_LINE;
        case TT_DRAWER_OPEN:      return EXT_DRAWER_OPEN;
        case TT_DRAWER_CLOSE:     return EXT_DRAWER_CLOSE;
        case TT_GBLOCK_OPEN:      return EXT_GBLOCK_OPEN;
        case TT_GBLOCK_CLOSE:     return EXT_GBLOCK_CLOSE;
        /* TT_LBLOCK_OPEN / TT_LBLOCK_CLOSE are dispatched to the
         * type-specific tokens (src/example/export/verse/comment) by
         * the scanner main body, not here.  TT_LBLOCK_BODY is shared. */
        case TT_LBLOCK_BODY:      return EXT_LBLOCK_BODY;
        case TT_DYNBLOCK_OPEN:    return EXT_DYNBLOCK_OPEN;
        case TT_DYNBLOCK_CLOSE:   return EXT_DYNBLOCK_CLOSE;
        case TT_LATEXENV_OPEN:    return EXT_LATEXENV_OPEN;
        case TT_LATEXENV_BODY:    return EXT_LATEXENV_BODY;
        case TT_LATEXENV_CLOSE:   return EXT_LATEXENV_CLOSE;
        case TT_KEYWORD:          return EXT_KEYWORD_LINE;
        case TT_AFFILIATED_KEYWORD: return EXT_AFFILIATED_KEYWORD_LINE;
        case TT_COMMENT:          return EXT_COMMENT_LINE;
        case TT_FIXED_WIDTH:      return EXT_FIXED_WIDTH_LINE;
        case TT_HRULE:            return EXT_HRULE_LINE;
        /* TT_TABLE_ROW maps to EXT_TABLE_ROW_START via dedicated handling
         * in the scanner main loop — we emit cell-by-cell tokens, not a
         * single line token, so this fall-through is intentionally absent. */
        case TT_TABLE_RULE:       return EXT_TABLE_RULE_LINE;
        /* TT_LIST_ITEM is handled inline before line classification — the
         * scanner emits EXT_LIST_ITEM_BULLET for just the bullet bytes,
         * leaving content on the line for `_inline_content_line` matches. */
        case TT_FOOTNOTE_DEF:     return EXT_FOOTNOTE_DEF_LINE;
        case TT_INLINETASK_OPEN:  return EXT_INLINETASK_OPEN;
        case TT_INLINETASK_CLOSE: return EXT_INLINETASK_CLOSE;
        case TT_CLOCK:            return EXT_CLOCK_LINE;
        case TT_DIARY_SEXP:       return EXT_DIARY_SEXP_LINE;
        case TT_BODY:             return EXT_INLINE_CONTENT_LINE;
        case TT_EMPTY:            return EXT_EMPTY_LINE;
        default:                  return -1;
    }
}

#define ORG_LINE_BUF_MAX  8192
#define ORG_HEADING_STACK 32
#define ORG_LIST_STACK    32

typedef struct {
    prepass_state_t *prepass;
    /* Levels of open headings, innermost last.  Full at 32 entries
     * (a serialized depth of exactly ORG_HEADING_STACK is valid);
     * deeper headings are tracked by the parser but not the stack. */
    uint8_t          heading_levels[ORG_HEADING_STACK];
    uint8_t          heading_depth;
    /* When the scanner encounters a heading line it may need to emit
     * one or more zero-width _heading_close tokens before the
     * _heading_open for that line.  These fields queue that work. */
    uint8_t          pending_closes;      /* close tokens still to emit */
    uint8_t          pending_open_level;  /* 0 = none; >0 = level of queued open */

    /* List indent stack — each open `plain_list` records the indent
     * column of its items.  When a bullet at smaller indent appears (or
     * a non-bullet line interrupts the list), we pop entries and emit
     * `_plain_list_close` zero-width tokens.  When a bullet at greater
     * indent appears, we push and emit `_plain_list_open`. */
    uint8_t          list_indents[ORG_LIST_STACK];
    uint8_t          list_depth;
    uint8_t          pending_list_closes;     /* close tokens still to emit */
    int16_t          pending_list_open_indent; /* -1 = none; else indent to push */

    /* Lesser-block kind currently open (so the matching #+end_NAME line
     * dispatches to the right close token).  Lesser blocks cannot nest,
     * so a single byte suffices.
     *   0 = none, 1 = src, 2 = example, 3 = export, 4 = verse, 5 = comment */
    uint8_t          lblock_kind;

    /* Set right after a description-list tag separator (` :: `) is
     * emitted: the next line content is the item's definition, on the
     * same line, so it must be emitted as paragraph content rather than
     * run through heading / bullet / line classification (which would
     * mis-handle a definition that starts with `*` / `+` / `|` / `#`).
     * Consumed (and cleared) by the very next scan. */
    uint8_t          at_item_def;

    /* Consecutive empty lines emitted so far.  Two consecutive
     * blanks terminate plain lists and footnote definitions. */
    uint8_t          blank_run;

    /* Per-instance line-read scratch (never serialized); keeps the
     * scanner reentrant across parser instances.  Must stay the
     * last members: scanner_state_clear memsets only up to here. */
    uint8_t          row_buf[ORG_LINE_BUF_MAX];
    uint8_t          line_buf[ORG_LINE_BUF_MAX];
    uint8_t          line_buf2[ORG_LINE_BUF_MAX];
    uint8_t          fn_line_buf[ORG_LINE_BUF_MAX];
} ScannerState;

/* Find the byte index of the block name in `#+begin_NAME ...` or
 * `#+end_NAME ...` lines.  Returns the position after `#+begin_` /
 * `#+end_`, or 0 if not a block line. */
static uint32_t lblock_name_offset(const uint8_t *buf, uint32_t n) {
    /* Skip leading whitespace. */
    uint32_t i = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i + 8 <= n && (buf[i] == '#') && buf[i + 1] == '+') {
        const uint8_t *p = buf + i + 2;
        uint32_t rem = n - (i + 2);
        if (rem >= 6 && (p[0] == 'b' || p[0] == 'B')
            && (p[1] == 'e' || p[1] == 'E')
            && (p[2] == 'g' || p[2] == 'G')
            && (p[3] == 'i' || p[3] == 'I')
            && (p[4] == 'n' || p[4] == 'N')
            && p[5] == '_') {
            return i + 8;
        }
        if (rem >= 4 && (p[0] == 'e' || p[0] == 'E')
            && (p[1] == 'n' || p[1] == 'N')
            && (p[2] == 'd' || p[2] == 'D')
            && p[3] == '_') {
            return i + 6;
        }
    }
    return 0;
}

void *tree_sitter_org_external_scanner_create(void) {
    ScannerState *s = (ScannerState *)calloc(1, sizeof(ScannerState));
    if (!s) return NULL;
    s->prepass = prepass_state_new();
    if (!s->prepass) { free(s); return NULL; }
    s->pending_list_open_indent = -1;
    return s;
}

void tree_sitter_org_external_scanner_destroy(void *payload) {
    ScannerState *s = (ScannerState *)payload;
    if (!s) return;
    prepass_state_free(s->prepass);
    free(s);
}

unsigned tree_sitter_org_external_scanner_serialize(void *payload, char *buffer) {
    ScannerState *s  = (ScannerState *)payload;
    uint8_t      *buf = (uint8_t *)buffer;
    size_t        cap = TREE_SITTER_SERIALIZATION_BUFFER_SIZE;

    /* Binary layout:
     *   [0]                       heading_depth
     *   [..]                      heading_levels[heading_depth]
     *   [..]                      pending_closes
     *   [..]                      pending_open_level
     *   [..]                      list_depth
     *   [..]                      list_indents[list_depth]
     *   [..]                      pending_list_closes
     *   [.. 2 bytes LE]           pending_list_open_indent
     *   [..]                      lblock_kind
     *   [..]                      at_item_def
     *   [..]                      blank_run
     *   [..]                      prepass scope stack (depth byte + entries)
     */
    size_t hdr = 10u + (size_t)s->heading_depth + (size_t)s->list_depth;
    if (hdr > cap) return 0;

    size_t pos = 0;
    buf[pos++] = s->heading_depth;
    if (s->heading_depth > 0) {
        memcpy(buf + pos, s->heading_levels, s->heading_depth);
        pos += s->heading_depth;
    }
    buf[pos++] = s->pending_closes;
    buf[pos++] = s->pending_open_level;
    buf[pos++] = s->list_depth;
    if (s->list_depth > 0) {
        memcpy(buf + pos, s->list_indents, s->list_depth);
        pos += s->list_depth;
    }
    buf[pos++] = s->pending_list_closes;
    /* int16_t little-endian */
    buf[pos++] = (uint8_t)(s->pending_list_open_indent & 0xff);
    buf[pos++] = (uint8_t)((s->pending_list_open_indent >> 8) & 0xff);
    buf[pos++] = s->lblock_kind;
    buf[pos++] = s->at_item_def;
    buf[pos++] = s->blank_run;

    size_t pp_n = prepass_serialize(s->prepass, buf + pos, cap - pos);
    if (pp_n > cap - pos) return 0;
    return (unsigned)(pos + pp_n);
}

static void scanner_state_clear(ScannerState *s) {
    prepass_state_t *pp = s->prepass;
    memset(s, 0, offsetof(ScannerState, row_buf));
    s->prepass = pp;
    s->pending_list_open_indent = -1;
    prepass_reset(s->prepass);
}

void tree_sitter_org_external_scanner_deserialize(void *payload,
                                                   const char *buffer,
                                                   unsigned length) {
    ScannerState    *s   = (ScannerState *)payload;
    const uint8_t   *buf = (const uint8_t *)buffer;

    scanner_state_clear(s);
    if (length == 0) return;

    size_t pos = 0;
    uint8_t hdepth = buf[pos++];
    if (hdepth > ORG_HEADING_STACK) goto corrupt;
    if (length < (size_t)hdepth + 8) goto corrupt;

    s->heading_depth = hdepth;
    if (hdepth > 0) {
        memcpy(s->heading_levels, buf + pos, hdepth);
        pos += hdepth;
    }
    s->pending_closes     = buf[pos++];
    s->pending_open_level = buf[pos++];

    {
        uint8_t ldepth = buf[pos++];
        if (ldepth > ORG_LIST_STACK) goto corrupt;
        if (length < pos + ldepth + 3) goto corrupt;

        s->list_depth = ldepth;
        if (ldepth > 0) {
            memcpy(s->list_indents, buf + pos, ldepth);
            pos += ldepth;
        }
    }
    s->pending_list_closes = buf[pos++];
    s->pending_list_open_indent =
        (int16_t)((uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8));
    pos += 2;
    if (length < pos + 3) goto corrupt;
    s->lblock_kind = buf[pos++];
    s->at_item_def = buf[pos++];
    s->blank_run   = buf[pos++];

    if (length <= pos) goto corrupt;
    if (!prepass_deserialize(s->prepass, buf + pos, (size_t)length - pos))
        goto corrupt;
    return;

corrupt:
    scanner_state_clear(s);
}

/* -----------------------------------------------------------------------
 * Heading close/open state machine helpers
 * --------------------------------------------------------------------- */

/* Count how many entries on the heading stack have level >= `level`.
 * These must be closed before a new headline at `level` can open. */
static uint8_t closes_needed(const ScannerState *s, uint8_t level) {
    uint8_t n = 0;
    for (int i = (int)s->heading_depth - 1; i >= 0; i--) {
        if (s->heading_levels[i] >= level) n++;
        else break;
    }
    return n;
}

/* External close symbol that terminates the innermost open scope, or
 * -1 when there is none.  Used to truncate an unclosed container
 * (block / drawer / inlinetask) when a headline or EOF interrupts it:
 * the close token is emitted zero-width and the scope popped, one
 * scope per scan, so the parser and the prepass stack stay in
 * lockstep across nesting. */
static int scope_close_symbol(const ScannerState *s, ScopeKind top) {
    switch (top) {
        case SCOPE_LBLOCK:
            switch (s->lblock_kind) {
                case 1:  return EXT_SRC_BLOCK_CLOSE;
                case 2:  return EXT_EXAMPLE_BLOCK_CLOSE;
                case 3:  return EXT_EXPORT_BLOCK_CLOSE;
                case 4:  return EXT_VERSE_BLOCK_CLOSE;
                case 5:  return EXT_COMMENT_BLOCK_CLOSE;
                default: return -1;
            }
        case SCOPE_GBLOCK:     return EXT_GBLOCK_CLOSE;
        case SCOPE_DYNBLOCK:   return EXT_DYNBLOCK_CLOSE;
        case SCOPE_LATEXENV:   return EXT_LATEXENV_CLOSE;
        case SCOPE_DRAWER:     return EXT_DRAWER_CLOSE;
        case SCOPE_PROPDRAWER: return EXT_PROPDRAWER_CLOSE;
        case SCOPE_INLINETASK: return EXT_INLINETASK_CLOSE;
        default:               return -1;
    }
}

/* Emit the zero-width close for the innermost open scope if the
 * parser can accept it.  Shared by the headline-interrupt and EOF
 * paths; the caller must have mark_end at the truncation point. */
static bool close_innermost_scope(ScannerState *s, TSLexer *lexer,
                                  const bool *valid_symbols) {
    ScopeKind top = prepass_scope_top(s->prepass);
    int close_sym = scope_close_symbol(s, top);
    if (close_sym < 0 || !valid_symbols[close_sym]) return false;
    prepass_scope_pop(s->prepass);
    if (top == SCOPE_LBLOCK) s->lblock_kind = 0;
    lexer->result_symbol = (TSSymbol)close_sym;
    return true;
}

/* ---------------------------------------------------------------------------
 * Headline-line sub-scanners.
 *
 * `_heading_open` covers only the leading `*+` stars. After that, the JS
 * rule for `headline_line` parses:
 *
 *     headline_line: stars + ws + priority? + title? + tag_list?
 *
 * `priority` is a simple regex; `title` and `tag_list` have a context-
 * sensitive boundary — title ends at either end-of-line OR the start of
 * a trailing `:tag1:tag2:` block. Tree-sitter regex can't express that
 * (no lookahead), so we handle title + tag_list here.
 *
 * Tree-sitter mark_end semantics: mark_end records token end. Calls to
 * advance after mark_end move the lexer forward, but on scan() return
 * true the lexer position is reset to the LAST mark_end call. We use
 * this to peek-then-rewind: advance through a candidate tag region;
 * on success set mark_end to the position BEFORE the tag region.
 * --------------------------------------------------------------------- */

static inline bool is_tag_char(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '_' || c == '@' || c == '#' || c == '%';
}

static inline bool is_alpha(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

/* From the current lexer position, ADVANCE through a candidate tag
 * region (`:tag:tag:[ws]*` ending at newline/EOF) and return true if
 * we found one. On success the lexer is past the region (caller can
 * use mark_end to bound the actual emitted token). */
static bool consume_tag_region(TSLexer *lexer) {
    if (lexer->lookahead != ':') return false;
    lexer->advance(lexer, false);  /* opening `:` */
    if (!is_tag_char(lexer->lookahead)) return false;
    while (true) {
        while (is_tag_char(lexer->lookahead)) lexer->advance(lexer, false);
        if (lexer->lookahead != ':') return false;
        lexer->advance(lexer, false);  /* closing `:` of this tag */
        int32_t la = lexer->lookahead;
        if (is_tag_char(la)) continue;          /* `:tag1:tag2:` next iter */
        /* End of tag region. Skip trailing inline whitespace then
         * verify we reached end-of-line. */
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
            lexer->advance(lexer, false);
        return lexer->lookahead == '\n' || lexer->lookahead == '\r'
            || lexer->lookahead == 0 || lexer->eof(lexer);
    }
}

/* Peek-test a statistics cookie at current position: `[N%]` or
 * `[N/M]`, valid ONLY when followed (after inline whitespace) by
 * end-of-line or a tag region - Emacs treats a mid-title cookie as
 * plain text.  Advances past the candidate; on true the caller must
 * not mark_end past it.  `*last_consumed` tracks the last byte
 * advanced past (including the trailing ws-skip loop) so a failed
 * caller can reseed its own boundary check from the true preceding
 * byte instead of the post-failure lookahead. */
static bool consume_stats_cookie(TSLexer *lexer, int32_t *last_consumed) {
    if (lexer->lookahead != '[') return false;
    *last_consumed = lexer->lookahead;
    lexer->advance(lexer, false);
    while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);
    }
    int32_t c = lexer->lookahead;
    if (c == '%') {
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);
        if (lexer->lookahead != ']') return false;
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);
    } else if (c == '/') {
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);
        while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
            *last_consumed = lexer->lookahead;
            lexer->advance(lexer, false);
        }
        if (lexer->lookahead != ']') return false;
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);
    } else {
        return false;
    }
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);
    }
    if (lexer->lookahead == '\n' || lexer->lookahead == '\r'
        || lexer->lookahead == 0 || lexer->eof(lexer))
        return true;
    if (lexer->lookahead == ':') return consume_tag_region(lexer);
    return false;
}

/* Scan a headline title from the current position to end-of-line OR
 * the start of a trailing tag region OR a statistics cookie. The
 * title token covers everything UP TO the cookie / tag boundary
 * marker (`[` or `:` preceded by ws); the lexer rewinds to that
 * position so the next external scanner can fire on the marker
 * char directly. Trailing ws right before the boundary is included
 * in the title (Emacs convention; consumers may trim). */
/* Scan title content from the current position to end-of-line, a
 * valid tag region, or a trailing statistics cookie.  `prev0` seeds
 * the boundary check (callers mid-word pass their current
 * lookahead; a line-start caller passes '\n').  Returns true when
 * any title bytes were marked, including `any0` from bytes the
 * caller already consumed and marked. */
static bool scan_title_tail(TSLexer *lexer, int32_t prev0, bool any0) {
    bool any_title_chars = any0;
    int32_t prev = prev0;
    while (true) {
        int32_t c = lexer->lookahead;
        if (c == '\n' || c == '\r' || c == 0 || lexer->eof(lexer)) break;
        if (c == ':' && (prev == ' ' || prev == '\t')) {
            if (consume_tag_region(lexer)) return any_title_chars;
            any_title_chars = true;
            lexer->mark_end(lexer);
            prev = lexer->lookahead;
            continue;
        }
        if (c == '[' && (prev == ' ' || prev == '\t')) {
            int32_t last_consumed = c;
            if (consume_stats_cookie(lexer, &last_consumed)) return any_title_chars;
            any_title_chars = true;
            lexer->mark_end(lexer);
            prev = last_consumed;
            continue;
        }
        lexer->advance(lexer, false);
        any_title_chars = true;
        lexer->mark_end(lexer);
        prev = c;
    }
    return any_title_chars;
}

static bool scan_headline_title(TSLexer *lexer) {
    return scan_title_tail(lexer, '\n', false);
}

/* Eat trailing inline whitespace after a bullet cookie so the
 * paragraph's first `_inline_content_line` token starts at the real
 * text, then mark the token end. */
static void finish_list_cookie(TSLexer *lexer) {
    lexer->mark_end(lexer);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
        lexer->advance(lexer, false);
    lexer->mark_end(lexer);
}

/* Bounded append; bytes beyond cap are consumed by the lexer but
 * dropped from the classification buffer. */
static inline void push_byte(uint8_t *buf, uint32_t cap, uint32_t *len,
                             uint8_t b) {
    if (*len < cap) buf[(*len)++] = b;
}

/* Classification buffers hold one byte per codepoint; any non-ASCII
 * codepoint becomes 0x80, which matches no structural prefix test. */
static inline uint8_t classify_byte(int32_t la) {
    return (la >= 0 && la < 0x80) ? (uint8_t)la : 0x80;
}

/* ASCII-CI planning / clock keyword match on klen bytes.
 * Returns 0 = none, 1 = planning (SCHEDULED / DEADLINE / CLOSED),
 * 2 = clock (CLOCK).  Emacs matches these with case-fold-search. */
static int planning_clock_kw(const uint8_t *p, uint32_t klen) {
    static const struct { const char *kw; uint32_t len; int kind; } KWS[] = {
        { "scheduled", 9, 1 },
        { "deadline",  8, 1 },
        { "closed",    6, 1 },
        { "clock",     5, 2 },
    };
    for (int k = 0; k < 4; k++) {
        if (klen != KWS[k].len) continue;
        bool eq = true;
        for (uint32_t i = 0; i < klen; i++) {
            uint8_t a = p[i];
            if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
            if (a != (uint8_t)KWS[k].kw[i]) { eq = false; break; }
        }
        if (eq) return KWS[k].kind;
    }
    return 0;
}

/* Keyword kind for a line prefix ending at its ':' (buf[len-1]).
 * Returns 0 = none, 1 = planning, 2 = clock. */
static int line_planning_clock_kind(const uint8_t *buf, uint32_t len) {
    if (len < 6 || buf[len - 1] != ':') return 0;
    uint32_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    return planning_clock_kw(buf + i, (len - 1) - i);
}

/* ASCII case-insensitive prefix match: does p[0..len) start with kw? */
static bool prefix_ci(const uint8_t *p, uint32_t len, const char *kw) {
    size_t klen = strlen(kw);
    if (len < klen) return false;
    for (size_t i = 0; i < klen; i++) {
        uint8_t a = p[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
        if (a != (uint8_t)kw[i]) return false;
    }
    return true;
}

/* True when buf[after_colon..len) starts (after inline whitespace)
 * with a well-formed <...> or [...] timestamp.  Mirrors the grammar
 * regex: angle form may not contain '<' or '>', bracket form may
 * not contain ']'.  Emacs treats a planning keyword without a valid
 * timestamp as plain paragraph text. */
static bool planning_timestamp_follows(const uint8_t *buf, uint32_t len,
                                       uint32_t after_colon) {
    uint32_t i = after_colon;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i >= len) return false;
    if (buf[i] == '<') {
        uint32_t j = i + 1;
        while (j < len && buf[j] != '<' && buf[j] != '>') j++;
        return j > i + 1 && j < len && buf[j] == '>';
    }
    if (buf[i] == '[') {
        uint32_t j = i + 1;
        while (j < len && buf[j] != ']') j++;
        return j > i + 1 && j < len;
    }
    return false;
}

/* Counter `[@N]` and checkbox `[ ]`/`[x]`/`[X]`/`[-]` both start with
 * `[` at the same list-item position (counter precedes checkbox).  A
 * single `scan()` call cannot roll back between two separate attempts,
 * so consume the `[` once and branch on the next char.  Returns the
 * matched external symbol, or -1.  Per spec/org.abnf:
 * counter = "[@" ["start:"] (1*DIGIT / ALPHA) "]" — a digit run or a
 * single letter, with an optional `start:` prefix, matching Emacs
 * `org-list-full-item-re`. */
static int scan_list_bracket_cookie(TSLexer *lexer, const bool *valid_symbols) {
    if (lexer->lookahead != '[') return -1;
    lexer->advance(lexer, false);
    int32_t c = lexer->lookahead;
    if (c == '@' && valid_symbols[EXT_LIST_COUNTER]) {
        lexer->advance(lexer, false);
        char run[8];
        int n = 0;
        while ((is_alpha(lexer->lookahead) || is_digit(lexer->lookahead))
               && n < (int)sizeof(run)) {
            run[n++] = (char)lexer->lookahead;
            lexer->advance(lexer, false);
        }
        /* Optional `start:` prefix introduces the real value. */
        if (n == 5 && run[0]=='s' && run[1]=='t' && run[2]=='a'
            && run[3]=='r' && run[4]=='t' && lexer->lookahead == ':') {
            lexer->advance(lexer, false);
            n = 0;
            while ((is_alpha(lexer->lookahead) || is_digit(lexer->lookahead))
                   && n < (int)sizeof(run)) {
                run[n++] = (char)lexer->lookahead;
                lexer->advance(lexer, false);
            }
        }
        /* Value is a single letter or a run of digits. */
        bool ok = false;
        if (n == 1 && is_alpha((int32_t)run[0])) {
            ok = true;
        } else if (n >= 1) {
            ok = true;
            for (int i = 0; i < n; i++)
                if (!is_digit((int32_t)run[i])) { ok = false; break; }
        }
        if (!ok || lexer->lookahead != ']') return -1;
        lexer->advance(lexer, false);
        finish_list_cookie(lexer);
        return EXT_LIST_COUNTER;
    }
    if ((c == ' ' || c == 'x' || c == 'X' || c == '-')
        && valid_symbols[EXT_LIST_CHECKBOX]) {
        lexer->advance(lexer, false);
        if (lexer->lookahead != ']') return -1;
        lexer->advance(lexer, false);
        finish_list_cookie(lexer);
        return EXT_LIST_CHECKBOX;
    }
    return -1;
}

/* Zero-width validator: returns true iff the current position starts
 * a valid tag region (`:tag1:tag2:[ws]*` ending at newline/EOF).
 * The internal advances during validation are discarded by tree-sitter
 * on return false; on return true, mark_end was called at start so the
 * lexer position rewinds to the `:` and the JS rule consumes it. */
static bool scan_tag_list_open(TSLexer *lexer) {
    if (lexer->lookahead != ':') return false;
    lexer->mark_end(lexer);  /* zero-width emit */
    return consume_tag_region(lexer);
}

/* Scan a priority cookie `[#X]` where X is uppercase letter or digit. */
static bool scan_headline_priority(TSLexer *lexer) {
    if (lexer->lookahead != '[') return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != '#') return false;
    lexer->advance(lexer, false);
    int32_t c = lexer->lookahead;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!ok) return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != ']') return false;
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    return true;
}

/* -----------------------------------------------------------------------
 * Main scanner
 * --------------------------------------------------------------------- */

/* Prefix-mark kinds shared by the line-read loops: which structural
 * prefix (if any) the emitted token should cover. */
enum LineMark { MARK_NONE, MARK_HASH, MARK_LBLOCK, MARK_COLON,
                MARK_DRAWER, MARK_PROPERTY, MARK_PLANNING, MARK_CLOCK,
                MARK_INLINETASK, MARK_DIARY_SEXP, MARK_COMMENT_LINE,
                MARK_GBLOCK, MARK_DYNBLOCK, MARK_LATEXENV };

/* Kind of the lesser-block name starting at `off`, or 0.  Scans only
 * the name-char run (stopping at the first byte outside
 * [a-zA-Z0-9_-] or at `n`) and matches that run exactly - bytes
 * beyond the run, up to `n`, are ignored.  This leniency is required
 * by the TT_LBLOCK_OPEN dispatch call sites, which pass the full
 * line (name plus any trailing switches/args) as `n`.  Callers that
 * read a line incrementally and need to know the name run reached
 * exactly to `n` (nothing pending after it yet) should pass a
 * non-NULL `end_out` and compare `*end_out == n` themselves. */
static uint8_t lblock_kind_at(const uint8_t *buf, uint32_t off, uint32_t n,
                              uint32_t *end_out) {
    uint32_t end = off;
    while (end < n && ((buf[end] >= 'a' && buf[end] <= 'z')
                       || (buf[end] >= 'A' && buf[end] <= 'Z')
                       || (buf[end] >= '0' && buf[end] <= '9')
                       || buf[end] == '_' || buf[end] == '-')) end++;
    if (end_out) *end_out = end;
    return prepass_lblock_kind(buf, off, end);
}

static bool ws_only_before(const uint8_t *buf, uint32_t end) {
    for (uint32_t i = 0; i < end; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') return false;
    }
    return true;
}

static bool scan_impl(ScannerState *s, TSLexer *lexer,
                      const bool *valid_symbols);

bool tree_sitter_org_external_scanner_scan(void *payload, TSLexer *lexer,
                                             const bool *valid_symbols) {
    ScannerState *s = (ScannerState *)payload;
    bool ok = scan_impl(s, lexer, valid_symbols);
    if (ok) {
        if (lexer->result_symbol == EXT_EMPTY_LINE
            || lexer->result_symbol == EXT_FN_EMPTY_LINE) {
            if (s->blank_run < 255) s->blank_run++;
        } else {
            s->blank_run = 0;
        }
    }
    return ok;
}

static bool scan_impl(ScannerState *s, TSLexer *lexer,
                      const bool *valid_symbols) {

    /* Diary-sexp body: everything up to (not including) the line's
     * LAST `)` - Emacs reads to the outermost closing paren, so
     * nested parens stay in the body.  Empty bodies are refused
     * (grammar marks the field optional), which also keeps this
     * token from ever being zero-width. */
    if (valid_symbols[EXT_DIARY_SEXP_BODY]) {
        if (lexer->lookahead == ')') return false;
        bool marked = false;
        lexer->mark_end(lexer);
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && lexer->lookahead != '\r') {
            if (lexer->lookahead == ')') {
                lexer->mark_end(lexer);
                marked = true;
            }
            lexer->advance(lexer, false);
        }
        if (!marked) return false;
        lexer->result_symbol = EXT_DIARY_SEXP_BODY;
        return true;
    }

    /* ── Priority 0z: body-text token for comment_line / fixed_width_line.
     * Fires when the parser, having just consumed a prefix-only
     * `_comment_line` / `_fixed_width_line` token, asks for the body
     * field.  We read until end-of-line, marking_end after each non-
     * whitespace byte so trailing whitespace is excluded.  Returning
     * `false` (no body content) lets the parser skip the optional
     * field and match the trailing newline regex. */
    if (valid_symbols[EXT_COMMENT_BODY_TEXT]
        || valid_symbols[EXT_FIXED_WIDTH_BODY_TEXT]) {
        int sym = valid_symbols[EXT_COMMENT_BODY_TEXT]
                    ? EXT_COMMENT_BODY_TEXT : EXT_FIXED_WIDTH_BODY_TEXT;
        bool any_non_ws = false;
        /* Skip past leading whitespace, but include it in the token if
         * non-ws content follows (so `# foo` body is ` foo` — including
         * the leading space — which is the natural body slice). */
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && lexer->lookahead != '\r') {
            int32_t la = lexer->lookahead;
            lexer->advance(lexer, false);
            if (la != ' ' && la != '\t') {
                any_non_ws = true;
                lexer->mark_end(lexer);
            }
        }
        if (!any_non_ws) return false;
        lexer->result_symbol = (TSSymbol)sym;
        return true;
    }

    /* ── Priority 0a: list counter `[@N]` / checkbox `[ ]`. Both fire
     * only when valid (i.e. right after the bullet's ws). */
    if ((valid_symbols[EXT_LIST_COUNTER] || valid_symbols[EXT_LIST_CHECKBOX])
        && lexer->lookahead == '[') {
        int sym = scan_list_bracket_cookie(lexer, valid_symbols);
        if (sym >= 0) {
            lexer->result_symbol = (TSSymbol)sym;
            return true;
        }
    }

    /* ── Priority 0a: description-list tag separator ` :: `.  Valid only
     * right after a _item_tag_text token, where the separator is
     * guaranteed present (the term emit confirmed it).  Consumes the
     * whitespace + `::` + trailing whitespace so the definition content
     * follows. */
    if (valid_symbols[EXT_ITEM_TAG_SEP]
        && (lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
            lexer->advance(lexer, false);
        if (lexer->lookahead == ':') {
            lexer->advance(lexer, false);
            if (lexer->lookahead == ':') {
                lexer->advance(lexer, false);
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                    lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                if (lexer->lookahead == '\n' || lexer->lookahead == '\r'
                    || lexer->eof(lexer)) {
                    /* Empty same-line definition: pull the lone newline into
                     * the separator so the item ends with no paragraph (or
                     * the definition continues on the next line). */
                    if (lexer->lookahead == '\r') lexer->advance(lexer, false);
                    if (lexer->lookahead == '\n') lexer->advance(lexer, false);
                    lexer->mark_end(lexer);
                } else {
                    /* Real definition on this line — the next scan emits it. */
                    s->at_item_def = 1;
                }
                lexer->result_symbol = EXT_ITEM_TAG_SEP;
                return true;
            }
        }
    }

    /* ── Priority 0b: a description item's definition (the content after
     * ` :: `, on the same line).  Flagged by the separator emit above.
     * Like the item's first content line it is paragraph text, so emit it
     * directly instead of letting heading / bullet / line classification
     * mis-handle a `*` / `+` / `|` / `#`-leading definition. */
    if (s->at_item_def) {
        s->at_item_def = 0;
        /* Emit the rest of the line (including an empty definition's lone
         * newline, so it is consumed like a normal content line) as the
         * definition's first paragraph line.  At EOF there is nothing to
         * consume, so fall through and let the item end with no paragraph. */
        if (valid_symbols[EXT_INLINE_CONTENT_LINE] && !lexer->eof(lexer)) {
            while (!lexer->eof(lexer) && lexer->lookahead != '\n')
                lexer->advance(lexer, false);
            if (!lexer->eof(lexer) && lexer->lookahead == '\n')
                lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
        }
    }

    /* ── Priority 0b: a list item's first content line.  EXT_ITEM_TAG_TEXT
     * is valid only right after the bullet (and any counter / checkbox),
     * where the rest of the line is the item's content — never a heading /
     * bullet / table / comment.  Take over here, before the structural
     * detectors, so content beginning with a metacharacter (`*`, `+`, `-`,
     * `|`, `#`, digits) stays item text.  A ` :: ` separator makes it a
     * description item: emit the TERM as _item_tag_text (the separator is
     * re-lexed as _item_tag_sep).  Otherwise emit the whole line as the
     * first paragraph line.  The TERM is the shortest match (first
     * separator), matching Emacs `org-list-full-item-re`. */
    if (valid_symbols[EXT_ITEM_TAG_TEXT]
        && valid_symbols[EXT_INLINE_CONTENT_LINE]
        && lexer->lookahead != '\n' && lexer->lookahead != '\r'
        && !lexer->eof(lexer)) {
        bool any = false;     /* a non-ws term char has been seen */
        bool marked = false;  /* mark_end sits at a term-end candidate */
        for (;;) {
            int32_t c = lexer->lookahead;
            if (c == '\n' || c == '\r' || lexer->eof(lexer)) break;
            if (c == ' ' || c == '\t') {
                if (any) { lexer->mark_end(lexer); marked = true; }
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                    lexer->advance(lexer, false);
                if (lexer->lookahead == ':') {
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == ':') {
                        lexer->advance(lexer, false);
                        int32_t a2 = lexer->lookahead;
                        if ((a2 == ' ' || a2 == '\t' || a2 == '\n'
                             || a2 == '\r' || lexer->eof(lexer))
                            && any && marked) {
                            lexer->result_symbol = EXT_ITEM_TAG_TEXT;
                            return true;
                        }
                    }
                }
                continue;
            }
            lexer->advance(lexer, false);
            any = true;
        }
        /* No separator — the whole line is the first paragraph line. */
        if (!lexer->eof(lexer) && lexer->lookahead == '\r')
            lexer->advance(lexer, false);
        if (!lexer->eof(lexer) && lexer->lookahead == '\n')
            lexer->advance(lexer, false);
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
        return true;
    }

    /* ── Priority 0: headline_line sub-tokens. Tree-sitter restores
     * lexer state on scan() return false but NOT between sub-scanner
     * attempts within the same call. So we pick exactly ONE token
     * based on lookahead, optionally falling through to title for
     * "candidate todo that turns out not to be one" without losing
     * the consumed chars. */
    if (valid_symbols[EXT_HEADLINE_TODO] || valid_symbols[EXT_HEADLINE_TITLE]
        || valid_symbols[EXT_HEADLINE_PRIORITY]
        || valid_symbols[EXT_HEADLINE_COMMENT]
        || valid_symbols[EXT_HEADLINE_STATS_COOKIE]
        || valid_symbols[EXT_HEADLINE_TAG_LIST_OPEN]) {
        int32_t la = lexer->lookahead;

        /* Priority's separator is optional, so a COMMENT marker can be
         * the very next symbol while `la` still sits on that separator
         * whitespace. Defer to the internal `/[ \t]+/` token instead of
         * folding it into title, so the next scan starts at the word
         * and can classify it as COMMENT. This guard is what keeps the
         * shared `/[ \t]+/` separator token in grammar.js's headline
         * rules safe: tree-sitter interns identical anonymous tokens,
         * so editing those separators or this guard requires the other. */
        if ((la == ' ' || la == '\t') && valid_symbols[EXT_HEADLINE_COMMENT]) {
            return false;
        }

        /* `[` — could be priority `[#X]`, cookie `[N%]` / `[N/M]`,
         * or part of the title. We commit to advancing past `[`,
         * peek the next char to decide which branch, and either
         * emit that token OR fall through to title (with the `[`
         * already included). Note: tree-sitter rollback on false
         * return only happens BETWEEN scan() calls, not within. */
        if (la == '['
            && (valid_symbols[EXT_HEADLINE_PRIORITY]
                || valid_symbols[EXT_HEADLINE_STATS_COOKIE])) {
            lexer->advance(lexer, false);  /* past `[` */
            int32_t c1 = lexer->lookahead;
            /* Seeds scan_title_tail's boundary check on fallthrough;
             * the mid-title-cookie path below overrides this with the
             * true last-consumed byte once it skips trailing ws. */
            int32_t fallback_prev = c1;
            /* Priority `[#X]`. */
            if (c1 == '#' && valid_symbols[EXT_HEADLINE_PRIORITY]) {
                lexer->advance(lexer, false);
                int32_t cx = lexer->lookahead;
                if ((cx >= 'A' && cx <= 'Z') || (cx >= '0' && cx <= '9')) {
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == ']') {
                        lexer->advance(lexer, false);
                        lexer->mark_end(lexer);
                        lexer->result_symbol = EXT_HEADLINE_PRIORITY;
                        return true;
                    }
                }
                /* malformed — fall through to title fallback below */
            }
            /* Cookie `[N%]` / `[N/M]`. */
            else if (((c1 >= '0' && c1 <= '9') || c1 == '%' || c1 == '/')
                     && valid_symbols[EXT_HEADLINE_STATS_COOKIE]) {
                while (lexer->lookahead >= '0' && lexer->lookahead <= '9')
                    lexer->advance(lexer, false);
                int32_t c = lexer->lookahead;
                bool ok = false;
                if (c == '%') {
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == ']') {
                        lexer->advance(lexer, false); ok = true;
                    }
                } else if (c == '/') {
                    lexer->advance(lexer, false);
                    while (lexer->lookahead >= '0' && lexer->lookahead <= '9')
                        lexer->advance(lexer, false);
                    if (lexer->lookahead == ']') {
                        lexer->advance(lexer, false); ok = true;
                    }
                }
                if (ok) {
                    fallback_prev = ']';
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        fallback_prev = lexer->lookahead;
                        lexer->advance(lexer, false);
                    }
                    if (lexer->lookahead == '\n' || lexer->lookahead == '\r'
                        || lexer->lookahead == 0 || lexer->eof(lexer)) {
                        lexer->mark_end(lexer);
                        lexer->result_symbol = EXT_HEADLINE_STATS_COOKIE;
                        return true;
                    }
                    if (lexer->lookahead == ':') {
                        /* Mark before probing so the tag validator's
                         * advances are outside the token; the next
                         * scan re-lexes the tags. */
                        lexer->mark_end(lexer);
                        if (consume_tag_region(lexer)) {
                            lexer->result_symbol = EXT_HEADLINE_STATS_COOKIE;
                            return true;
                        }
                    }
                    /* Mid-title cookie: fall through to the title
                     * fallback below with the bytes already consumed. */
                }
                /* malformed — fall through to title fallback below */
            }
            /* Neither matched — `[...` is plain title content. We've
             * already advanced past `[`; continue title scan from here
             * and include those bytes in the title token. */
            if (!valid_symbols[EXT_HEADLINE_TITLE]) return false;
            lexer->mark_end(lexer);
            scan_title_tail(lexer, fallback_prev, true);
            lexer->result_symbol = EXT_HEADLINE_TITLE;
            return true;
        }

        /* Priority cookie. */
        if (la == '[' && valid_symbols[EXT_HEADLINE_PRIORITY]) {
            if (scan_headline_priority(lexer)) {
                lexer->result_symbol = EXT_HEADLINE_PRIORITY;
                return true;
            }
            /* If priority validation failed mid-advance, fall through
             * to title (which can include `[` as ordinary content). */
            if (valid_symbols[EXT_HEADLINE_TITLE]) {
                if (scan_headline_title(lexer)) {
                    lexer->result_symbol = EXT_HEADLINE_TITLE;
                    return true;
                }
            }
            return false;
        }

        /* Tag list opener (zero-width validator). */
        if (la == ':' && valid_symbols[EXT_HEADLINE_TAG_LIST_OPEN]) {
            if (scan_tag_list_open(lexer)) {
                lexer->result_symbol = EXT_HEADLINE_TAG_LIST_OPEN;
                return true;
            }
            /* `:` here is not a tag region — treat as title content. */
            if (valid_symbols[EXT_HEADLINE_TITLE]) {
                if (scan_headline_title(lexer)) {
                    lexer->result_symbol = EXT_HEADLINE_TITLE;
                    return true;
                }
            }
            return false;
        }

        /* TODO / COMMENT / title disambiguation. All can start with
         * uppercase letters; we advance through the word once and
         * decide based on what we matched.
         *
         * Order: COMMENT (literal "COMMENT") wins over generic TODO.
         * Both require trailing whitespace. If neither matches, the
         * consumed chars fold into the title. */
        if (la >= 'A' && la <= 'Z'
            && (valid_symbols[EXT_HEADLINE_TODO]
                || valid_symbols[EXT_HEADLINE_COMMENT]
                || valid_symbols[EXT_HEADLINE_TITLE])) {
            /* Buffer the word so we can string-compare against COMMENT. */
            char word[16];
            int wlen = 0;
            int letters = 0;
            while (lexer->lookahead >= 'A' && lexer->lookahead <= 'Z') {
                if (wlen < 15) word[wlen++] = (char)lexer->lookahead;
                lexer->advance(lexer, false); letters++;
            }
            while ((lexer->lookahead >= 'A' && lexer->lookahead <= 'Z')
                   || (lexer->lookahead >= '0' && lexer->lookahead <= '9')
                   || lexer->lookahead == '_'
                   || lexer->lookahead == '-') {
                if (wlen < 15) word[wlen++] = (char)lexer->lookahead;
                lexer->advance(lexer, false);
            }
            word[wlen] = '\0';
            bool ws_after = (lexer->lookahead == ' ' || lexer->lookahead == '\t');
            /* COMMENT: exact match + ws. */
            if (valid_symbols[EXT_HEADLINE_COMMENT]
                && ws_after && wlen == 7
                && word[0] == 'C' && word[1] == 'O' && word[2] == 'M'
                && word[3] == 'M' && word[4] == 'E' && word[5] == 'N'
                && word[6] == 'T') {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_HEADLINE_COMMENT;
                return true;
            }
            /* TODO: >= 2 uppercase letters at start + ws. */
            if (valid_symbols[EXT_HEADLINE_TODO] && letters >= 2 && ws_after) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_HEADLINE_TODO;
                return true;
            }
            /* Not TODO/COMMENT — fall through to title. */
            if (!valid_symbols[EXT_HEADLINE_TITLE]) return false;
            lexer->mark_end(lexer);
            scan_title_tail(lexer, lexer->lookahead, true);
            lexer->result_symbol = EXT_HEADLINE_TITLE;
            return true;
        }

        /* Default: title (lowercase / digits / punctuation start). */
        if (valid_symbols[EXT_HEADLINE_TITLE]) {
            if (scan_headline_title(lexer)) {
                lexer->result_symbol = EXT_HEADLINE_TITLE;
                return true;
            }
        }
    }

    /* ── Priority 1: drain queued _heading_close tokens (zero-width). ── */
    if (s->pending_closes > 0 && valid_symbols[EXT_HEADING_CLOSE]) {
        lexer->mark_end(lexer);   /* token end = start → zero-width */
        s->pending_closes--;
        s->heading_depth--;
        lexer->result_symbol = EXT_HEADING_CLOSE;
        return true;
    }

    /* ── Priority 1.5: drain queued list closes (zero-width). ────────── */
    if (s->pending_list_closes > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
        lexer->mark_end(lexer);
        s->pending_list_closes--;
        s->list_depth--;
        lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
        return true;
    }

    /* ── Priority 1.6: drain queued list open (zero-width). ──────────── */
    if (s->pending_list_open_indent >= 0 && valid_symbols[EXT_PLAIN_LIST_OPEN]) {
        lexer->mark_end(lexer);
        if (s->list_depth < ORG_LIST_STACK)
            s->list_indents[s->list_depth++] = (uint8_t)s->pending_list_open_indent;
        s->pending_list_open_indent = -1;
        lexer->result_symbol = EXT_PLAIN_LIST_OPEN;
        return true;
    }

    /* ── Priority 2: emit queued _heading_open (covers the stars). ──
     * The OPEN token covers the leading `*+` of the heading line. JS
     * rules then consume the rest (space + priority + title + tags +
     * newline) and expose them as named nodes. The lookahead is at
     * the line-start position (heading_close was zero-width), so we
     * advance past `level` stars before mark_end. */
    if (s->pending_open_level > 0 && valid_symbols[EXT_HEADING_OPEN]) {
        uint8_t level = s->pending_open_level;
        s->pending_open_level = 0;
        for (uint8_t i = 0; i < level; i++) {
            if (lexer->lookahead != '*') break;
            lexer->advance(lexer, false);
        }
        lexer->mark_end(lexer);
        if (s->heading_depth < ORG_HEADING_STACK)
            s->heading_levels[s->heading_depth++] = level;
        lexer->result_symbol = EXT_HEADING_OPEN;
        return true;
    }

    /* ── Priority 3: EOF — close all remaining open lists, then any
     * still-open containers (an unclosed block / drawer / inlinetask
     * runs to EOF), then headings. ── */
    if (lexer->eof(lexer)) {
        lexer->mark_end(lexer);
        if (s->list_depth > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
            s->list_depth--;
            lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
            return true;
        }
        if (close_innermost_scope(s, lexer, valid_symbols)) {
            return true;
        }
        if (s->heading_depth > 0 && valid_symbols[EXT_HEADING_CLOSE]) {
            s->heading_depth--;
            lexer->result_symbol = EXT_HEADING_CLOSE;
            return true;
        }
        return false;
    }

    if (s->list_depth > 0
        && valid_symbols[EXT_PLAIN_LIST_CLOSE]
        && lexer->get_column(lexer) == 0) {
        int32_t la = lexer->lookahead;
        bool maybe_bullet = (la == ' ' || la == '\t'
                              || la == '-' || la == '+'
                              || (la >= '0' && la <= '9'));
        bool blank = (la == '\n' || la == '\r');
        /* A single blank line stays inside the list; the SECOND
         * consecutive blank terminates it (Emacs org-list-end-re).
         * `*` at column 0 is never a list bullet, so any other
         * non-bullet line still ends the list immediately. */
        if ((blank && s->blank_run >= 1) || (!maybe_bullet && !blank)) {
            lexer->mark_end(lexer);
            s->list_depth--;
            lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
            return true;
        }
    }

    /* ── Priority 4: detect heading line; emit close(s) then open. ───── */
    uint8_t consumed_stars = 0;
    if (lexer->lookahead == '*') {
        bool at_line_start = lexer->get_column(lexer) == 0;

        /* Call mark_end before advancing, so that if we emit a
         * _heading_close the token is zero-width and the next scan()
         * call will re-present the same line for the _heading_open. */
        lexer->mark_end(lexer);

        /* Count leading stars. */
        uint8_t level = 0;
        while (!lexer->eof(lexer) && lexer->lookahead == '*') {
            if (level < 255) level++;
            lexer->advance(lexer, false);
        }

        bool is_heading = (level > 0 && level < ORG_INLINETASK_MIN_LEVEL
                           && !lexer->eof(lexer)
                           && lexer->lookahead == ' ');

        if (is_heading) {
            /* A column-0 headline terminates any container it appears
             * in — block, drawer, or inlinetask (Emacs parity:
             * `org-at-heading-p` is t inside #+begin_.../#+end_...; a
             * literal star must be escaped `,*`).  The grammar
             * requires the container's close token, so emit it
             * zero-width here — mark_end is still at line start, so
             * the heading line is re-presented on the next scan and
             * the close/open dance below runs with the container
             * reduced. */
            if (at_line_start
                && close_innermost_scope(s, lexer, valid_symbols)) {
                return true;
            }

            uint8_t cn = closes_needed(s, level);

            /* Closes must come first. */
            if (cn > 0 && valid_symbols[EXT_HEADING_CLOSE]) {
                s->pending_closes     = cn - 1;
                s->pending_open_level = level;
                s->heading_depth--;
                lexer->result_symbol  = EXT_HEADING_CLOSE;
                /* mark_end was called at start → zero-width token */
                return true;
            }

            /* No closes; emit OPEN covering only the stars. The JS
             * rules for `headline_line` consume the rest (space +
             * priority + title + tag_list + newline) so the parse
             * tree exposes those as named nodes. mark_end was called
             * at line start (above); calling it again here moves the
             * end to the current position (after stars). */
            if (valid_symbols[EXT_HEADING_OPEN]) {
                lexer->mark_end(lexer);  /* token spans the stars */
                if (s->heading_depth < ORG_HEADING_STACK)
                    s->heading_levels[s->heading_depth++] = level;
                lexer->result_symbol = EXT_HEADING_OPEN;
                return true;
            }

            /* Heading detected but heading_open isn't valid yet — we're
             * inside a still-open list (or nested lists).  Emit a
             * `_plain_list_close` zero-width to peel one level off; on
             * subsequent scans the same heading line will be re-presented
             * (mark_end at start) and detection re-runs. */
            if (s->list_depth > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
                s->list_depth--;
                lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
                return true;
            }
        }
        /* Not a heading OR fell through (lesser-block scope, etc.).
         * We already advanced past `level` stars; record so we can
         * prepend them to the body line buffer below. */
        consumed_stars = level;
    }

    /* ── Priority 4a: mid-row table tokens (pipe / cell content / row end).
     * Only one of these symbols is in valid_symbols at a time when the
     * parser is partway through a `table_row`.  We dispatch by lookahead.
     * ─────────────────────────────────────────────────────────────────── */
    if (consumed_stars == 0
        && (valid_symbols[EXT_TABLE_PIPE]
            || valid_symbols[EXT_TABLE_CELL_CONTENT]
            || valid_symbols[EXT_TABLE_ROW_END])) {

        if ((lexer->lookahead == '\n' || lexer->lookahead == '\r')
            && valid_symbols[EXT_TABLE_ROW_END]) {
            if (lexer->lookahead == '\r') lexer->advance(lexer, false);
            if (lexer->lookahead == '\n') lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_TABLE_ROW_END;
            return true;
        }
        if (lexer->lookahead == '|' && valid_symbols[EXT_TABLE_PIPE]) {
            lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_TABLE_PIPE;
            return true;
        }
        if (valid_symbols[EXT_TABLE_CELL_CONTENT]
            && lexer->lookahead != '|'
            && lexer->lookahead != '\n'
            && lexer->lookahead != '\r'
            && !lexer->eof(lexer)) {
            while (!lexer->eof(lexer)
                   && lexer->lookahead != '|'
                   && lexer->lookahead != '\n'
                   && lexer->lookahead != '\r') {
                lexer->advance(lexer, false);
            }
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_TABLE_CELL_CONTENT;
            return true;
        }
        /* No applicable token; fall through. */
    }

    /* ── Priority 4b: start of a new table row or rule.  We enter here when
     * the parser is at a row-start position and lookahead is `|`.  We
     * advance one byte (past the leading `|`), mark_end there, then peek
     * the rest of the line into a buffer to decide between row and rule. */
    if (consumed_stars == 0
        && (valid_symbols[EXT_TABLE_ROW_START] || valid_symbols[EXT_TABLE_RULE_LINE])
        && lexer->lookahead == '|') {

        lexer->advance(lexer, false);
        lexer->mark_end(lexer);   /* position after the leading | */

        uint8_t *row_buf = s->row_buf;
        uint32_t row_len = 1;
        row_buf[0] = '|';
        while (!lexer->eof(lexer)
               && lexer->lookahead != '\n'
               && row_len < ORG_LINE_BUF_MAX) {
            row_buf[row_len++] = classify_byte(lexer->lookahead);
            lexer->advance(lexer, false);
        }
        if (row_len > 0 && row_buf[row_len - 1] == '\r') row_len--;

        PrepassScopeSnapshot snap = prepass_scope_snapshot(s->prepass);
        LineClassification r =
            prepass_classify_line(s->prepass, row_buf, row_len);

        if (r.type == TT_TABLE_ROW && valid_symbols[EXT_TABLE_ROW_START]) {
            /* mark_end is still at position 1 — token spans just the `|`. */
            lexer->result_symbol = EXT_TABLE_ROW_START;
            return true;
        }
        if (r.type == TT_TABLE_RULE && valid_symbols[EXT_TABLE_RULE_LINE]) {
            if (!lexer->eof(lexer) && lexer->lookahead == '\n')
                lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_TABLE_RULE_LINE;
            return true;
        }
        /* Classification didn't match what valid_symbols allowed.  Returning
         * false aborts; tree-sitter restores the lexer to the original
         * position (before the leading `|` advance). */
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    /* ── Priority 4c: list-item bullet detection.  Emits a token covering
     * exactly the indent + bullet chars + trailing whitespace.  Supports
     * `- `, `+ `, `* ` (only at indent > 0), and `N.` / `N)` numeric
     * bullets.  `[@N]` counters and `[X]` checkboxes following the bullet
     * are emitted as their own tokens (see Priority 0a).
     *
     * Gated to column 0: bullets and list open/close are line-based, so
     * this must only fire at a line start.  Without the guard it also
     * fired on an item's mid-line content right after the bullet token
     * (column > 0) — content beginning with a digit / `-` / `+` / `*`
     * was then mis-parsed (e.g. detached from the item as a list-ending
     * line). */
    if (consumed_stars == 0
        && lexer->get_column(lexer) == 0
        && (valid_symbols[EXT_LIST_ITEM_BULLET]
            || valid_symbols[EXT_PLAIN_LIST_OPEN]
            || valid_symbols[EXT_PLAIN_LIST_CLOSE])
        && (lexer->lookahead == ' ' || lexer->lookahead == '\t'
            || lexer->lookahead == '-' || lexer->lookahead == '+'
            || lexer->lookahead == '*'
            || (lexer->lookahead >= '0' && lexer->lookahead <= '9'))) {

        /* CRITICAL: mark_end at start so a zero-width close/open is possible
         * on this line.  We'll re-call mark_end if we need a non-zero-width
         * bullet token. */
        lexer->mark_end(lexer);

        uint8_t bullet_consumed[64];
        uint32_t bullet_consumed_len = 0;

        /* Skip indent. */
        uint32_t indent = 0;
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            push_byte(bullet_consumed, sizeof(bullet_consumed),
                      &bullet_consumed_len, (uint8_t)lexer->lookahead);
            lexer->advance(lexer, false);
            indent++;
        }

        bool ok = false;
        int32_t la = lexer->lookahead;

        if (la == '-' || la == '+' || (la == '*' && indent > 0)) {
            push_byte(bullet_consumed, sizeof(bullet_consumed),
                      &bullet_consumed_len, (uint8_t)la);
            lexer->advance(lexer, false);
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                push_byte(bullet_consumed, sizeof(bullet_consumed),
                          &bullet_consumed_len, (uint8_t)lexer->lookahead);
                lexer->advance(lexer, false);
                ok = true;
            }
        } else if (la >= '0' && la <= '9') {
            while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
                push_byte(bullet_consumed, sizeof(bullet_consumed),
                          &bullet_consumed_len, (uint8_t)lexer->lookahead);
                lexer->advance(lexer, false);
            }
            if (lexer->lookahead == '.' || lexer->lookahead == ')') {
                push_byte(bullet_consumed, sizeof(bullet_consumed),
                          &bullet_consumed_len, (uint8_t)lexer->lookahead);
                lexer->advance(lexer, false);
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    push_byte(bullet_consumed, sizeof(bullet_consumed),
                              &bullet_consumed_len, (uint8_t)lexer->lookahead);
                    lexer->advance(lexer, false);
                    ok = true;
                }
            }
        }

        if (ok) {
            uint8_t cur_indent = (uint8_t)indent;

            /* Compute closes_needed against current list stack. */
            uint8_t closes_needed = 0;
            uint8_t depth = s->list_depth;
            while (depth > 0 && s->list_indents[depth - 1] > cur_indent) {
                closes_needed++;
                depth--;
            }
            bool need_open = (depth == 0
                              || s->list_indents[depth - 1] < cur_indent);

            /* Emit close first (zero-width — mark_end is still at start). */
            if (closes_needed > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
                s->pending_list_closes = closes_needed - 1;
                s->pending_list_open_indent = need_open ? (int16_t)cur_indent : -1;
                s->list_depth--;
                lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
                return true;
            }

            /* No closes; emit open if needed (zero-width). */
            if (need_open && valid_symbols[EXT_PLAIN_LIST_OPEN]) {
                if (s->list_depth < ORG_LIST_STACK)
                    s->list_indents[s->list_depth++] = cur_indent;
                lexer->result_symbol = EXT_PLAIN_LIST_OPEN;
                return true;
            }

            /* No close, no open — emit bullet directly.  Update mark_end to
             * end of the consumed bytes. */
            if (valid_symbols[EXT_LIST_ITEM_BULLET]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_LIST_ITEM_BULLET;
                return true;
            }

            /* Fall through if none valid (shouldn't happen in well-formed input). */
        }

        /* Not a real bullet.  A continuation line must be indented
         * strictly more than the item's bullet column; otherwise the
         * list ends here (Emacs). */
        if (s->list_depth > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
            uint8_t top = s->list_indents[s->list_depth - 1];
            if ((uint32_t)indent <= (uint32_t)top) {
                s->list_depth--;
                lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
                return true;
            }
        }

        /* Fall through to line classification with the consumed bytes
         * prepended so the prepass sees the full line. */
        uint8_t *line_buf2 = s->line_buf2;
        uint32_t ll = 0;
        for (uint32_t i = 0; i < bullet_consumed_len && ll < ORG_LINE_BUF_MAX; i++)
            line_buf2[ll++] = bullet_consumed[i];

        /* Indented table row or rule (Priority 4b only fires at
         * column 0).  Diverts only when the parser can take a table
         * token; inside a list item the classification fallback
         * below keeps the line paragraph text. */
        if (lexer->lookahead == '|'
            && (valid_symbols[EXT_TABLE_ROW_START]
                || valid_symbols[EXT_TABLE_RULE_LINE])) {
            if (ll < ORG_LINE_BUF_MAX) line_buf2[ll++] = '|';
            lexer->advance(lexer, false);
            lexer->mark_end(lexer);   /* row token = indent + leading pipe */
            while (!lexer->eof(lexer) && lexer->lookahead != '\n'
                   && ll < ORG_LINE_BUF_MAX) {
                line_buf2[ll++] = classify_byte(lexer->lookahead);
                lexer->advance(lexer, false);
            }
            if (ll > 0 && line_buf2[ll - 1] == '\r') ll--;
            PrepassScopeSnapshot tsnap = prepass_scope_snapshot(s->prepass);
            LineClassification tr =
                prepass_classify_line(s->prepass, line_buf2, ll);
            if (tr.type == TT_TABLE_ROW && valid_symbols[EXT_TABLE_ROW_START]) {
                lexer->result_symbol = EXT_TABLE_ROW_START;
                return true;
            }
            if (tr.type == TT_TABLE_RULE && valid_symbols[EXT_TABLE_RULE_LINE]) {
                if (!lexer->eof(lexer) && lexer->lookahead == '\n')
                    lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_TABLE_RULE_LINE;
                return true;
            }
            prepass_scope_restore(s->prepass, tsnap);
            return false;
        }

        int  b2_mark = MARK_NONE;
        bool have_b2_mark = false;
        int  b2_forced_sym = -1;
        uint32_t b2_kw_colon = 0;
        lexer->mark_end(lexer);
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && ll < ORG_LINE_BUF_MAX) {
            line_buf2[ll++] = classify_byte(lexer->lookahead);
            lexer->advance(lexer, false);

            /* `:` after only whitespace: drawer / property /
             * fixed-width prefix - token covers just the colon. */
            if (b2_mark == MARK_NONE && b2_forced_sym < 0
                && line_buf2[ll - 1] == ':'
                && ws_only_before(line_buf2, ll - 1)) {
                lexer->mark_end(lexer);
                b2_mark = MARK_COLON;
                have_b2_mark = true;
                continue;
            }

            /* Lesser-block prefix (`#+begin_src` etc.); upgrades a
             * MARK_HASH set for the bare `#+`. */
            if (b2_mark != MARK_LBLOCK && b2_mark != MARK_COLON
                && b2_mark != MARK_COMMENT_LINE && b2_forced_sym < 0) {
                uint32_t off = lblock_name_offset(line_buf2, ll);
                if (off > 0) {
                    uint32_t name_end;
                    uint8_t kind = lblock_kind_at(line_buf2, off, ll, &name_end);
                    if (kind > 0 && name_end == ll) {
                        int32_t la2 = lexer->lookahead;
                        if (la2 == ' ' || la2 == '\t' || la2 == '\n'
                            || la2 == '\r' || la2 == 0 || lexer->eof(lexer)) {
                            lexer->mark_end(lexer);
                            b2_mark = MARK_LBLOCK;
                            have_b2_mark = true;
                            continue;
                        }
                    }
                    /* Greater block: mark right after `#+begin_`
                     * (or `#+end_`); a lesser-block name later
                     * upgrades the mark. */
                    if (b2_mark != MARK_GBLOCK && ll == off) {
                        lexer->mark_end(lexer);
                        b2_mark = MARK_GBLOCK;
                        have_b2_mark = true;
                        continue;
                    }
                }
            }

            /* Dynamic block open: mark right after `#+begin:`. */
            if (b2_mark == MARK_HASH && b2_forced_sym < 0 && ll >= 3
                && line_buf2[ll - 1] == ':') {
                uint32_t ws = 0;
                while (ws < ll
                       && (line_buf2[ws] == ' ' || line_buf2[ws] == '\t')) ws++;
                if (ll - ws == 8
                    && prefix_ci(line_buf2 + ws, ll - ws, "#+begin:")) {
                    lexer->mark_end(lexer);
                    b2_mark = MARK_DYNBLOCK;
                    have_b2_mark = true;
                    continue;
                }
            }

            /* Latex environment open: mark right after `\begin{`. */
            if (b2_mark == MARK_NONE && b2_forced_sym < 0 && ll >= 7
                && line_buf2[ll - 1] == '{') {
                uint32_t ws = 0;
                while (ws < ll
                       && (line_buf2[ws] == ' ' || line_buf2[ws] == '\t')) ws++;
                if (ll - ws == 7
                    && memcmp(line_buf2 + ws, "\\begin{", 7) == 0) {
                    lexer->mark_end(lexer);
                    b2_mark = MARK_LATEXENV;
                    have_b2_mark = true;
                    continue;
                }
            }

            if ((b2_mark != MARK_NONE && b2_mark != MARK_HASH
                 && b2_mark != MARK_GBLOCK) || b2_forced_sym >= 0) continue;

            /* Comment line: ws + `#` + non-`+`; token covers the `#`. */
            if (line_buf2[ll - 1] == '#'
                && ws_only_before(line_buf2, ll - 1)
                && lexer->lookahead != '+') {
                lexer->mark_end(lexer);
                b2_mark = MARK_COMMENT_LINE;
                have_b2_mark = true;
                continue;
            }

            /* Keyword / affiliated keyword `#+` prefix. */
            if (ll >= 2 && line_buf2[ll - 1] == '+'
                && line_buf2[ll - 2] == '#'
                && ws_only_before(line_buf2, ll - 2)) {
                lexer->mark_end(lexer);
                b2_mark = MARK_HASH;
                have_b2_mark = true;
                continue;
            }

            /* Planning / clock keyword ending at this `:`.  Token
             * covers the prefix through the colon (non-zero-width). */
            if (line_buf2[ll - 1] == ':' && ll >= 6) {
                int kw = line_planning_clock_kind(line_buf2, ll);
                if (kw != 0) {
                    lexer->mark_end(lexer);
                    b2_forced_sym = (kw == 2) ? EXT_CLOCK_LINE
                                              : EXT_PLANNING_LINE;
                    b2_kw_colon = ll;
                    have_b2_mark = true;
                    continue;
                }
            }
        }
        if (ll > 0 && line_buf2[ll - 1] == '\r') ll--;
        if (!lexer->eof(lexer) && lexer->lookahead == '\n')
            lexer->advance(lexer, false);
        if (!have_b2_mark) lexer->mark_end(lexer);

        if (b2_forced_sym >= 0) {
            if (b2_forced_sym == EXT_PLANNING_LINE
                && !planning_timestamp_follows(line_buf2, ll, b2_kw_colon)) {
                if (!valid_symbols[EXT_INLINE_CONTENT_LINE]) return false;
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            if (valid_symbols[b2_forced_sym]) {
                lexer->result_symbol = (TSSymbol)b2_forced_sym;
                return true;
            }
            /* Planning is only valid directly under a headline;
             * anywhere else the line is plain paragraph text. */
            if (b2_forced_sym == EXT_PLANNING_LINE
                && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            return false;
        }

        PrepassScopeSnapshot snap = prepass_scope_snapshot(s->prepass);
        LineClassification rr = prepass_classify_line(s->prepass, line_buf2, ll);
        if (rr.type == TT_HEADING) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        /* Close-line tokens with no JS-side tail cover the whole line. */
        if (rr.type == TT_DRAWER_CLOSE || rr.type == TT_PROPDRAWER_CLOSE
            || rr.type == TT_GBLOCK_CLOSE || rr.type == TT_DYNBLOCK_CLOSE)
            lexer->mark_end(lexer);
        if (rr.type == TT_LBLOCK_OPEN) {
            uint32_t off = lblock_name_offset(line_buf2, ll);
            uint8_t kind = off ? lblock_kind_at(line_buf2, off, ll, NULL) : 0;
            int open_sym = -1;
            switch (kind) {
                case 1: open_sym = EXT_SRC_BLOCK_OPEN;     break;
                case 2: open_sym = EXT_EXAMPLE_BLOCK_OPEN; break;
                case 3: open_sym = EXT_EXPORT_BLOCK_OPEN;  break;
                case 4: open_sym = EXT_VERSE_BLOCK_OPEN;   break;
                case 5: open_sym = EXT_COMMENT_BLOCK_OPEN; break;
                default: break;
            }
            if (open_sym >= 0 && valid_symbols[open_sym]) {
                s->lblock_kind = kind;
                lexer->result_symbol = (TSSymbol)open_sym;
                return true;
            }
            /* No slot for this block type here (e.g. a list item, whose
             * grammar only nests paragraph/list content) - same
             * graceful degradation as the table fallback below. */
            prepass_scope_restore(s->prepass, snap);
            if (valid_symbols[EXT_INLINE_CONTENT_LINE]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            return false;
        }
        if (rr.type == TT_LBLOCK_CLOSE) {
            int close_sym = -1;
            switch (s->lblock_kind) {
                case 1: close_sym = EXT_SRC_BLOCK_CLOSE;     break;
                case 2: close_sym = EXT_EXAMPLE_BLOCK_CLOSE; break;
                case 3: close_sym = EXT_EXPORT_BLOCK_CLOSE;  break;
                case 4: close_sym = EXT_VERSE_BLOCK_CLOSE;   break;
                case 5: close_sym = EXT_COMMENT_BLOCK_CLOSE; break;
                default: break;
            }
            if (close_sym >= 0 && valid_symbols[close_sym]) {
                s->lblock_kind = 0;
                lexer->result_symbol = (TSSymbol)close_sym;
                return true;
            }
            /* No open block to close here (e.g. the matching `#+begin_`
             * was itself downgraded to paragraph text above) - same
             * graceful degradation as the table fallback below. */
            prepass_scope_restore(s->prepass, snap);
            if (valid_symbols[EXT_INLINE_CONTENT_LINE]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            return false;
        }
        /* Table lines with no table slot here (e.g. inside a list
         * item) stay paragraph text. */
        if ((rr.type == TT_TABLE_ROW || rr.type == TT_TABLE_RULE)
            && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
        }
        int sym = prepass_to_external(rr.type);
        if (sym < 0) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        if (rr.type == TT_EMPTY && valid_symbols[EXT_FN_EMPTY_LINE]
            && s->blank_run == 0) {
            sym = EXT_FN_EMPTY_LINE;
        }
        if (!valid_symbols[sym]) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        lexer->result_symbol = (TSSymbol)sym;
        return true;
    }

    /* ── Priority 4d: footnote-def line — emit EXT_FOOTNOTE_DEF_LINE for
     * just `[fn:LABEL]`, leaving the rest of the line for
     * `_inline_content_line` to pick up.  Mirrors Emacs's
     * `org-element-footnote-definition-parser`, which treats anything
     * after `]` (including same-line text) as the body. */
    if (consumed_stars == 0
        && valid_symbols[EXT_FOOTNOTE_DEF_LINE]
        && lexer->lookahead == '[') {
        lexer->mark_end(lexer);  /* hold start position */

        uint8_t fn_consumed[260];
        uint32_t fn_len = 0;
        bool fn_ok = false;

        fn_consumed[fn_len++] = '[';
        lexer->advance(lexer, false);
        if (lexer->lookahead == 'f') {
            fn_consumed[fn_len++] = 'f';
            lexer->advance(lexer, false);
            if (lexer->lookahead == 'n') {
                fn_consumed[fn_len++] = 'n';
                lexer->advance(lexer, false);
                if (lexer->lookahead == ':') {
                    fn_consumed[fn_len++] = ':';
                    lexer->advance(lexer, false);
                    /* Mark after `[fn:` so JS rules consume label + `]`. */
                    lexer->mark_end(lexer);
                    uint32_t label_len = 0;
                    while (((lexer->lookahead >= 'A' && lexer->lookahead <= 'Z')
                            || (lexer->lookahead >= 'a' && lexer->lookahead <= 'z')
                            || (lexer->lookahead >= '0' && lexer->lookahead <= '9')
                            || lexer->lookahead == '_'
                            || lexer->lookahead == '-')
                           && fn_len < 255) {
                        fn_consumed[fn_len++] = (uint8_t)lexer->lookahead;
                        lexer->advance(lexer, false);
                        label_len++;
                    }
                    /* `[fn:LABEL]` is a definition; `[fn:LABEL:body]` is an
                     * inline reference — only the former gets the def
                     * treatment.  Reject `:` after the label. */
                    if (label_len > 0 && lexer->lookahead == ']') {
                        fn_consumed[fn_len++] = ']';
                        lexer->advance(lexer, false);
                        fn_ok = true;
                    }
                }
            }
        }

        if (fn_ok) {
            /* mark_end was set after `[fn:` above; keep it there so
             * the emitted `_footnote_def_line` covers only the prefix
             * and JS rules consume label + `]` as named children. */
            lexer->result_symbol = EXT_FOOTNOTE_DEF_LINE;
            return true;
        }

        /* Not actually a footnote-def.  Continue building the line buffer
         * with the speculatively-consumed bytes as prefix, then classify
         * normally — analogous to the list-bullet fall-through path. */
        uint8_t *fn_line_buf = s->fn_line_buf;
        uint32_t fn_ll = 0;
        for (uint32_t i = 0; i < fn_len && fn_ll < ORG_LINE_BUF_MAX; i++)
            fn_line_buf[fn_ll++] = fn_consumed[i];
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && fn_ll < ORG_LINE_BUF_MAX) {
            fn_line_buf[fn_ll++] = classify_byte(lexer->lookahead);
            lexer->advance(lexer, false);
        }
        if (fn_ll > 0 && fn_line_buf[fn_ll - 1] == '\r') fn_ll--;
        if (!lexer->eof(lexer) && lexer->lookahead == '\n')
            lexer->advance(lexer, false);
        lexer->mark_end(lexer);

        PrepassScopeSnapshot snap = prepass_scope_snapshot(s->prepass);
        LineClassification fr = prepass_classify_line(s->prepass, fn_line_buf, fn_ll);
        if (fr.type == TT_HEADING) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        int fsym = prepass_to_external(fr.type);
        if (fsym < 0) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        if (!valid_symbols[fsym]) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        lexer->result_symbol = (TSSymbol)fsym;
        return true;
    }

    /* ── Priority 5: classify line via prepass (non-heading). ────────── */
    uint8_t *line_buf = s->line_buf;
    uint32_t line_len = 0;

    for (uint8_t i = 0; i < consumed_stars && line_len < ORG_LINE_BUF_MAX; i++)
        line_buf[line_len++] = '*';

    if (lexer->eof(lexer) && line_len == 0)
        return false;

    /* Track sub-line mark_end positions. We snapshot mark_end during
     * the read to expose internal structure of single-line tokens.
     * Specificity order (later overrides earlier within the same
     * line read):
     *
     *   keyword / affiliated_keyword: cover `#+` (2 chars).
     *   lblock open: cover `#+begin_<name>` (more specific).
     *   drawer open: cover `:NAME:` (whole drawer marker).
     *   node_property: cover `:KEY:` (so JS picks up value).
     *
     * Once the most-specific marker has fired (lblock or drawer/
     * property), no later check runs.  If the prepass later
     * classifies the line as something we DIDN'T mark (e.g., a
     * paragraph that happens to start with `#+`), the post-loop
     * `if (!have_prefix_mark) lexer->mark_end(lexer);` extends to
     * end-of-line.
     */
    int mark_kind = MARK_NONE;
    uint32_t p5_kw_colon = 0;
    bool have_prefix_mark = false;
    /* Pre-mark at line start; mid-loop mark_end calls move this
     * forward as a prefix kind is detected. */
    lexer->mark_end(lexer);
    while (!lexer->eof(lexer) && lexer->lookahead != '\n'
           && line_len < ORG_LINE_BUF_MAX) {
        line_buf[line_len++] = classify_byte(lexer->lookahead);
        lexer->advance(lexer, false);

        /* `:`-leading line (after optional leading whitespace).  Mark
         * after the first `:` so the eventual `_drawer_open` /
         * `_node_property_line` / `_fixed_width_line` token covers
         * only that single colon; JS rules then consume the rest
         * (name + closing `:` + value, or body text). */
        if (mark_kind == MARK_NONE && line_buf[line_len - 1] == ':'
            && ws_only_before(line_buf, line_len - 1)) {
            lexer->mark_end(lexer);
            mark_kind = MARK_COLON;
            have_prefix_mark = true;
            continue;
        }

        /* Lblock open is the most specific `#+` form — always allow
         * upgrade from MARK_HASH to MARK_LBLOCK. */
        if (mark_kind != MARK_LBLOCK
            && mark_kind != MARK_DRAWER
            && mark_kind != MARK_PROPERTY) {
            uint32_t off = lblock_name_offset(line_buf, line_len);
            if (off > 0) {
                uint32_t name_end;
                uint8_t kind = lblock_kind_at(line_buf, off, line_len, &name_end);
                if (kind > 0 && name_end == line_len) {
                    int32_t la2 = lexer->lookahead;
                    if (la2 == ' ' || la2 == '\t' || la2 == '\n'
                        || la2 == '\r' || la2 == 0 || lexer->eof(lexer)) {
                        lexer->mark_end(lexer);
                        mark_kind = MARK_LBLOCK;
                        have_prefix_mark = true;
                        continue;
                    }
                }
                /* Greater block: mark right after `#+begin_` (or
                 * `#+end_`) so name and args become JS children;
                 * a lesser-block name later upgrades the mark. */
                if (mark_kind != MARK_GBLOCK && line_len == off) {
                    lexer->mark_end(lexer);
                    mark_kind = MARK_GBLOCK;
                    have_prefix_mark = true;
                    continue;
                }
            }
        }

        /* Dynamic block open: mark right after `#+begin:`. */
        if (mark_kind == MARK_HASH && line_len >= 3
            && line_buf[line_len - 1] == ':') {
            uint32_t ws = 0;
            while (ws < line_len
                   && (line_buf[ws] == ' ' || line_buf[ws] == '\t')) ws++;
            if (line_len - ws == 8
                && prefix_ci(line_buf + ws, line_len - ws, "#+begin:")) {
                lexer->mark_end(lexer);
                mark_kind = MARK_DYNBLOCK;
                have_prefix_mark = true;
                continue;
            }
        }

        /* Latex environment open: mark right after `\begin{`. */
        if (mark_kind == MARK_NONE && line_len >= 7
            && line_buf[line_len - 1] == '{') {
            uint32_t ws = 0;
            while (ws < line_len
                   && (line_buf[ws] == ' ' || line_buf[ws] == '\t')) ws++;
            if (line_len - ws == 7
                && memcmp(line_buf + ws, "\\begin{", 7) == 0) {
                lexer->mark_end(lexer);
                mark_kind = MARK_LATEXENV;
                have_prefix_mark = true;
                continue;
            }
        }

        if (mark_kind == MARK_LBLOCK
            || mark_kind == MARK_DRAWER
            || mark_kind == MARK_PROPERTY
            || mark_kind == MARK_PLANNING
            || mark_kind == MARK_CLOCK
            || mark_kind == MARK_INLINETASK
            || mark_kind == MARK_DIARY_SEXP
            || mark_kind == MARK_COMMENT_LINE
            || mark_kind == MARK_DYNBLOCK
            || mark_kind == MARK_LATEXENV) continue;

        /* Comment line: `#` followed by non-`+` (i.e. NOT a `#+keyword`
         * directive — that's caught by the MARK_HASH branch below).
         * Mark after `#` so the emitted `_comment_line` token covers
         * only the prefix; the JS `comment_line` rule consumes the
         * body text via `_comment_body_text` (an external token —
         * regex bodies caused a parse-table hang in the past). */
        if (mark_kind == MARK_NONE && line_len == 1 && line_buf[0] == '#'
            && lexer->lookahead != '+') {
            lexer->mark_end(lexer);
            mark_kind = MARK_COMMENT_LINE;
            have_prefix_mark = true;
            continue;
        }

        /* Diary sexp — bare `%%(...)` or active-form `<%%(...)>`.
         * Mark after the opening prefix so JS rules consume the body
         * + closing punctuation (and discriminate the trailing `>`). */
        if (mark_kind == MARK_NONE) {
            if (line_len == 3 && line_buf[0] == '%' && line_buf[1] == '%'
                && line_buf[2] == '(') {
                lexer->mark_end(lexer);
                mark_kind = MARK_DIARY_SEXP;
                have_prefix_mark = true;
                continue;
            }
            if (line_len == 4 && line_buf[0] == '<' && line_buf[1] == '%'
                && line_buf[2] == '%' && line_buf[3] == '(') {
                lexer->mark_end(lexer);
                mark_kind = MARK_DIARY_SEXP;
                have_prefix_mark = true;
                continue;
            }
        }

        /* Inlinetask open line: `***************<+> TITLE`.  Priority
         * 4 above pre-consumed the 15+ leading stars (and pre-filled
         * line_buf with them via consumed_stars), and the read loop
         * has just appended the trailing ' '. Detect that pattern and
         * back the mark_end up to the space position so JS rules can
         * reuse the heading-line externals for todo/priority/title. */
        if (mark_kind == MARK_NONE
            && line_buf[line_len - 1] == ' '
            && line_len >= ORG_INLINETASK_MIN_LEVEL + 1) {
            uint32_t star_count = 0;
            while (star_count < line_len - 1
                   && line_buf[star_count] == '*') star_count++;
            if (star_count >= ORG_INLINETASK_MIN_LEVEL
                && star_count == line_len - 1) {
                /* mark_end is currently AFTER the trailing space.
                 * That's actually fine — `_inlinetask_open` covers
                 * stars + 1 space; the JS rule just doesn't include
                 * a leading separator. */
                lexer->mark_end(lexer);
                mark_kind = MARK_INLINETASK;
                have_prefix_mark = true;
                continue;
            }
        }

        /* Keyword / affiliated keyword: `#+` prefix at line start. */
        if (mark_kind == MARK_NONE
            && line_len == 2 && line_buf[0] == '#' && line_buf[1] == '+') {
            lexer->mark_end(lexer);
            mark_kind = MARK_HASH;
            have_prefix_mark = true;
            continue;
        }

        /* Planning line keyword (`SCHEDULED:` / `DEADLINE:` / `CLOSED:`)
         * or clock line (`CLOCK:`). */
        if (mark_kind == MARK_NONE
            && line_buf[line_len - 1] == ':' && line_len >= 6) {
            int kw = line_planning_clock_kind(line_buf, line_len);
            if (kw != 0) {
                /* Token covers the prefix up to and including the ':'
                 * - non-zero-width, so error recovery always makes
                 * progress. */
                lexer->mark_end(lexer);
                mark_kind = (kw == 2) ? MARK_CLOCK : MARK_PLANNING;
                p5_kw_colon = line_len;
                have_prefix_mark = true;
                continue;
            }
        }

    }
    if (line_len > 0 && line_buf[line_len - 1] == '\r') line_len--;
    /* Consume the trailing newline (if any). */
    if (!lexer->eof(lexer) && lexer->lookahead == '\n')
        lexer->advance(lexer, false);

    /* Default token end = current position (whole line consumed).
     * If we DID set a prefix mark, that one stays in effect for an
     * lblock-open emit. */
    if (!have_prefix_mark) lexer->mark_end(lexer);

    if (mark_kind == MARK_PLANNING
        && !planning_timestamp_follows(line_buf, line_len, p5_kw_colon)) {
        if (!valid_symbols[EXT_INLINE_CONTENT_LINE]) return false;
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
        return true;
    }

    PrepassScopeSnapshot snap = prepass_scope_snapshot(s->prepass);
    LineClassification r = prepass_classify_line(s->prepass, line_buf, line_len);

    if (r.type == TT_HEADING) {
        /* Should be unreachable: heading detection above handles col-0 '*'
         * lines. This branch fires only if the prepass disagrees (shouldn't
         * happen) — return false to avoid confusing the grammar. */
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    /* Close-line tokens with no JS-side tail cover the whole line.
     * The mid-loop marks pinned them at a prefix (`:` for drawer
     * closes, stars + space for the inlinetask END line); extend to
     * the consumed end of line. */
    if (r.type == TT_DRAWER_CLOSE || r.type == TT_PROPDRAWER_CLOSE
        || r.type == TT_INLINETASK_CLOSE || r.type == TT_GBLOCK_CLOSE
        || r.type == TT_DYNBLOCK_CLOSE)
        lexer->mark_end(lexer);

    /* Lesser-block dispatch: emit one of 5 type-specific tokens based on
     * the block name.  TT_LBLOCK_OPEN sets `lblock_kind`; TT_LBLOCK_CLOSE
     * dispatches by that saved kind and clears it. */
    if (r.type == TT_LBLOCK_OPEN) {
        uint32_t off = lblock_name_offset(line_buf, line_len);
        uint8_t kind = off ? lblock_kind_at(line_buf, off, line_len, NULL) : 0;
        int open_sym = -1;
        switch (kind) {
            case 1: open_sym = EXT_SRC_BLOCK_OPEN;     break;
            case 2: open_sym = EXT_EXAMPLE_BLOCK_OPEN; break;
            case 3: open_sym = EXT_EXPORT_BLOCK_OPEN;  break;
            case 4: open_sym = EXT_VERSE_BLOCK_OPEN;   break;
            case 5: open_sym = EXT_COMMENT_BLOCK_OPEN; break;
            default:
                prepass_scope_restore(s->prepass, snap);
                return false;
        }
        if (!valid_symbols[open_sym]) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        s->lblock_kind = kind;
        lexer->result_symbol = (TSSymbol)open_sym;
        return true;
    }
    if (r.type == TT_LBLOCK_CLOSE) {
        int close_sym = -1;
        switch (s->lblock_kind) {
            case 1: close_sym = EXT_SRC_BLOCK_CLOSE;     break;
            case 2: close_sym = EXT_EXAMPLE_BLOCK_CLOSE; break;
            case 3: close_sym = EXT_EXPORT_BLOCK_CLOSE;  break;
            case 4: close_sym = EXT_VERSE_BLOCK_CLOSE;   break;
            case 5: close_sym = EXT_COMMENT_BLOCK_CLOSE; break;
            default:
                prepass_scope_restore(s->prepass, snap);
                return false;
        }
        if (!valid_symbols[close_sym]) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        s->lblock_kind = 0;
        lexer->result_symbol = (TSSymbol)close_sym;
        return true;
    }

    int sym = prepass_to_external(r.type);
    if (sym < 0) {
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    /* Inside a footnote definition body an empty line is the gated
     * _fn_empty_line token; refusing it on the second consecutive
     * blank forces the footnote_definition to end. */
    if (r.type == TT_EMPTY && valid_symbols[EXT_FN_EMPTY_LINE]
        && s->blank_run == 0) {
        sym = EXT_FN_EMPTY_LINE;
    }

    /* Override the prepass classification when our in-line detection
     * recognised a CLOCK / planning entry. Inside SCOPE_DRAWER the
     * prepass classifies everything except `:END:` as TT_BODY; that's
     * correct for arbitrary drawer content but loses CLOCK structure
     * for `:LOGBOOK:` which is the only place CLOCK lines live. */
    if (mark_kind == MARK_CLOCK && valid_symbols[EXT_CLOCK_LINE]) {
        sym = EXT_CLOCK_LINE;
    } else if (mark_kind == MARK_PLANNING && valid_symbols[EXT_PLANNING_LINE]) {
        sym = EXT_PLANNING_LINE;
    }
    if (sym == EXT_PLANNING_LINE && !valid_symbols[EXT_PLANNING_LINE]
        && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
        lexer->mark_end(lexer);
        sym = EXT_INLINE_CONTENT_LINE;
    }
    if (!valid_symbols[sym]) {
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    lexer->result_symbol = (TSSymbol)sym;
    return true;
}
