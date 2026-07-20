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
    EXT_FORMULA_LINE,            /* `#+TBLFM:` prefix (name confirmed here, not JS) */
    EXT_BLOCK_SWITCHES,          /* `-n 20 -r -l "fmt"` run after src/example language */
    EXT_EXPORT_FORMAT,           /* export-block backend, valid only if nothing but
                                   * trailing whitespace follows it to end of line */
    EXT_LINE_END,                /* `[ \t]*\r?\n`, or zero-width at EOF */
    EXT__COUNT,                  /* sentinel - keep last; not a real external symbol */
};

/* Number of real external symbols (excludes the EXT__COUNT sentinel
 * itself).  Anchored to the enum so a symbol appended without bumping
 * some separate manual constant cannot silently desync. */
#define N_EXTERNALS ((int)EXT__COUNT)

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
 * use mark_end to bound the actual emitted token).  `*last_consumed`
 * tracks the last byte advanced past, mirroring consume_stats_cookie,
 * so a failed caller can reseed its own boundary check from the true
 * preceding byte instead of the post-failure lookahead. */
static bool consume_tag_region(TSLexer *lexer, int32_t *last_consumed) {
    if (lexer->lookahead != ':') return false;
    *last_consumed = lexer->lookahead;
    lexer->advance(lexer, false);  /* opening `:` */
    if (!is_tag_char(lexer->lookahead)) return false;
    while (true) {
        while (is_tag_char(lexer->lookahead)) {
            *last_consumed = lexer->lookahead;
            lexer->advance(lexer, false);
        }
        if (lexer->lookahead != ':') return false;
        *last_consumed = lexer->lookahead;
        lexer->advance(lexer, false);  /* closing `:` of this tag */
        int32_t la = lexer->lookahead;
        if (is_tag_char(la)) continue;          /* `:tag1:tag2:` next iter */
        /* End of tag region. Skip trailing inline whitespace then
         * verify we reached end-of-line. */
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            *last_consumed = lexer->lookahead;
            lexer->advance(lexer, false);
        }
        return lexer->lookahead == '\n' || lexer->lookahead == '\r'
            || lexer->eof(lexer);
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
        || lexer->eof(lexer))
        return true;
    /* Thread our own out-param through: on failure here, the caller
     * needs the true last-consumed byte (not this function's stale
     * pre-":" value) to reseed its own boundary check. */
    if (lexer->lookahead == ':') return consume_tag_region(lexer, last_consumed);
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
            int32_t last_consumed = c;
            if (consume_tag_region(lexer, &last_consumed)) return any_title_chars;
            any_title_chars = true;
            lexer->mark_end(lexer);
            prev = last_consumed;
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
 * codepoint, and an embedded NUL (never EOF here - callers only call
 * this once eof() has already excluded that), becomes 0x80, which
 * matches no structural prefix test. */
static inline uint8_t classify_byte(int32_t la) {
    return (la > 0 && la < 0x80) ? (uint8_t)la : 0x80;
}

/* Peek past horizontal whitespace right after a `#+begin:` prefix to
 * see whether a dynamic-block name follows before EOL - Emacs
 * `org-dblock-start-re` requires one; a nameless line is a plain
 * keyword instead.  The whitespace is consumed into `buf` either way
 * (classification needs the full line regardless of outcome); the
 * lexer position after this call is the correct `mark_end()` boundary
 * for a genuine dynamic-block open (right before the name), and is
 * left untouched by the caller when no name follows, so an earlier
 * mark (the plain `#+` keyword prefix) stays in effect. */
static bool dynblock_name_follows(TSLexer *lexer, uint8_t *buf,
                                  uint32_t *len_io, uint32_t buf_max) {
    uint32_t len = *len_io;
    while (!lexer->eof(lexer)
           && (lexer->lookahead == ' ' || lexer->lookahead == '\t')
           && len < buf_max) {
        buf[len++] = classify_byte(lexer->lookahead);
        lexer->advance(lexer, false);
    }
    *len_io = len;
    return !lexer->eof(lexer)
        && lexer->lookahead != '\n' && lexer->lookahead != '\r';
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

/* True when a classified-keyword line's raw buffer is `#+TBLFM:` (name
 * exactly TBLFM, case-insensitive, immediately followed by `:` - no
 * indent-skip mismatch with the prepass's own `#+` detection).  A name
 * that merely starts with TBLFM (e.g. `#+TBLFMx:`) does not match. */
static bool is_tblfm_directive(const uint8_t *buf, uint32_t len) {
    uint32_t ws = 0;
    while (ws < len && (buf[ws] == ' ' || buf[ws] == '\t')) ws++;
    if (len - ws < 2 || buf[ws] != '#' || buf[ws + 1] != '+') return false;
    return prefix_ci(buf + ws + 2, len - ws - 2, "tblfm:");
}

/* Position of the next SCHEDULED:/DEADLINE:/CLOSED:/CLOCK: occurrence
 * (case-insensitive) at or after `start`, or `len` if none.  Bounds
 * the bracket-timestamp scan below so an unclosed `[` can't run past
 * a following entry's own keyword and validate against ITS closing
 * `]` instead. */
static uint32_t next_planning_kw_at(const uint8_t *buf, uint32_t start,
                                    uint32_t len) {
    for (uint32_t p = start; p < len; p++) {
        if (prefix_ci(buf + p, len - p, "scheduled:")) return p;
        if (prefix_ci(buf + p, len - p, "deadline:")) return p;
        if (prefix_ci(buf + p, len - p, "closed:")) return p;
        if (prefix_ci(buf + p, len - p, "clock:")) return p;
    }
    return len;
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
        uint32_t limit = next_planning_kw_at(buf, i + 1, len);
        uint32_t j = i + 1;
        while (j < limit && buf[j] != ']') j++;
        return j > i + 1 && j < limit && buf[j] == ']';
    }
    return false;
}

/* Matches the grammar's `clock_timestamp` (`/\[[^\]\n]+\]/`) starting
 * exactly at buf[i].  On success sets *end to the index past the
 * closing ']'.  Narrower than `planning_timestamp_follows`: no angle
 * form, and no next-keyword limit - a clock line has nothing else to
 * stop an unclosed '[' early, so an unclosed bracket simply fails. */
static bool clock_timestamp_at(const uint8_t *buf, uint32_t len, uint32_t i,
                               uint32_t *end) {
    if (i >= len || buf[i] != '[') return false;
    uint32_t j = i + 1;
    while (j < len && buf[j] != ']') j++;
    if (j == i + 1 || j >= len) return false;
    *end = j + 1;
    return true;
}

/* True when buf[after_colon..len) matches the clock line's full
 * remainder, with nothing left before the line end:
 *   [ \t]+ ts ( [ \t]*--[ \t]* ts ( [ \t]*=>[ \t]+ \d+:\d\d )? )? [ \t]*
 * Clock is all-or-nothing (contrast planning, whose trailing junk
 * after a valid timestamp is absorbed by the JS grammar instead of
 * invalidating the line) - any leftover, non-whitespace text before
 * the line end degrades the whole line to a paragraph. */
static bool clock_line_is_valid(const uint8_t *buf, uint32_t len,
                                uint32_t after_colon) {
    uint32_t i = after_colon;
    uint32_t ws_start = i;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i == ws_start) return false;

    uint32_t start_end;
    if (!clock_timestamp_at(buf, len, i, &start_end)) return false;
    i = start_end;

    uint32_t j = i;
    while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
    if (j + 1 < len && buf[j] == '-' && buf[j + 1] == '-') {
        uint32_t k = j + 2;
        while (k < len && (buf[k] == ' ' || buf[k] == '\t')) k++;
        uint32_t end_end;
        if (clock_timestamp_at(buf, len, k, &end_end)) {
            i = end_end;
            uint32_t m = i;
            while (m < len && (buf[m] == ' ' || buf[m] == '\t')) m++;
            if (m + 1 < len && buf[m] == '=' && buf[m + 1] == '>') {
                uint32_t n = m + 2;
                uint32_t ws2 = n;
                while (n < len && (buf[n] == ' ' || buf[n] == '\t')) n++;
                if (n > ws2) {
                    uint32_t d0 = n;
                    while (n < len && buf[n] >= '0' && buf[n] <= '9') n++;
                    if (n > d0 && n < len && buf[n] == ':'
                        && n + 2 < len
                        && buf[n + 1] >= '0' && buf[n + 1] <= '9'
                        && buf[n + 2] >= '0' && buf[n + 2] <= '9') {
                        i = n + 3;
                    }
                }
            }
        }
    }

    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    return i == len;
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
    int32_t last_consumed;
    return consume_tag_region(lexer, &last_consumed);
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

/* `block_switches`: one or more `-X`/`+X` atoms, each optionally
 * followed by a single argument (a run of `[ \t]` then a double-quoted
 * string or a run of digits), separated from the next atom by a run of
 * `[ \t]`.  Greedy: stops at the first byte that can't extend the run,
 * leaving it (and any whitespace already probed past looking for an
 * argument that wasn't there) for block_header_args.
 *
 * Implemented imperatively (not as JS regex tokens) because
 * tree-sitter's internal lexer commits to one token per merged DFA
 * state with no backtracking: a regex-only split cannot simultaneously
 * keep an existing multi-atom run intact (`-n 20 -r -l "fmt"`) and
 * hand a malformed run's tail (`-n-20 ...`) to block_header_args
 * without either an ERROR or the header-args catch-all swallowing a
 * valid leading run.
 *
 * `need_sep` tracks whether the next atom still needs its own
 * separator consumed: false right after a no-argument atom's
 * speculative whitespace probe already landed past the separator (the
 * probe had to consume it to see what followed); true right after a
 * matched argument, which leaves the cursor with no whitespace
 * consumed yet. */
static bool scan_block_switches(TSLexer *lexer) {
    bool any = false;
    bool need_sep = false;
    for (;;) {
        if (need_sep) {
            if (lexer->lookahead != ' ' && lexer->lookahead != '\t') break;
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                lexer->advance(lexer, false);
        }
        int32_t c = lexer->lookahead;
        if (c != '-' && c != '+') break;
        lexer->advance(lexer, false);
        c = lexer->lookahead;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) break;
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);  /* atom complete without an argument */
        any = true;
        need_sep = false;
        if (lexer->lookahead != ' ' && lexer->lookahead != '\t') continue;
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
            lexer->advance(lexer, false);
        c = lexer->lookahead;
        if (c >= '0' && c <= '9') {
            while (lexer->lookahead >= '0' && lexer->lookahead <= '9')
                lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            need_sep = true;
        } else if (c == '"') {
            lexer->advance(lexer, false);
            while (!lexer->eof(lexer) && lexer->lookahead != '\n'
                   && lexer->lookahead != '"')
                lexer->advance(lexer, false);
            if (lexer->lookahead == '"') {
                lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                need_sep = true;
            }
            /* unterminated quote: no re-mark, no need_sep - boundary
             * stays at the no-argument mark above; loops back and
             * tries the already-probed-past position directly as the
             * next atom's start. */
        }
        /* Whitespace not followed by a digit or quote: already
         * consumed probing for one - loops back and retries right
         * there as the next atom's start, no separator left to
         * consume. */
    }
    return any;
}

/* Export-block backend name (`html`, `latex`, ...): a single
 * non-whitespace run, valid ONLY when nothing but whitespace follows
 * it to end of line - mirrors Emacs's org-element-export-block-parser,
 * whose backend regex group is anchored `[ \t]+(\S-+))?[ \t]*$` against
 * the whole line. `#+begin_export html <b>` therefore has no backend
 * at all in Emacs (:type nil): the entire tail, "html <b>", falls
 * through to block_header_args instead of splitting into a format plus
 * leftover. Declining (false) here with no mark_end reached discards
 * all movement, same rollback-on-false contract as every other
 * external in this file. */
static bool scan_export_format(TSLexer *lexer) {
    if (lexer->lookahead == ' ' || lexer->lookahead == '\t'
        || lexer->lookahead == '\r' || lexer->lookahead == '\n'
        || lexer->eof(lexer)) return false;
    while (!lexer->eof(lexer) && lexer->lookahead != ' '
           && lexer->lookahead != '\t' && lexer->lookahead != '\r'
           && lexer->lookahead != '\n')
        lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
        lexer->advance(lexer, false);
    if (!lexer->eof(lexer) && lexer->lookahead != '\r'
        && lexer->lookahead != '\n')
        return false;
    return true;
}

/* Line terminator: [ \t]*CR?LF, or zero-width at EOF so EOF-truncated
 * lines close cleanly instead of erroring.  Must not consume anything
 * when it does not emit. */
static bool scan_line_end(TSLexer *lexer) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
        lexer->advance(lexer, false);
    if (lexer->lookahead == '\r') lexer->advance(lexer, false);
    if (lexer->lookahead == '\n') {
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_LINE_END;
        return true;
    }
    if (lexer->eof(lexer)) {
        /* At EOF the token covers the trailing [ \t]* run (zero-width
         * when the line ends flush at EOF). */
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_LINE_END;
        return true;
    }
    return false;
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

/* E7: forward look-ahead from a `:PROPERTIES:` open line through its
 * body, deciding whether `_propdrawer_open` may fire at all. Reads
 * lines via `lexer->advance()` without ever calling `lexer->mark_end()`,
 * so none of the peeked bytes are committed to the emitted token
 * regardless of the verdict - the caller re-scans them normally on the
 * next call. */
static bool propdrawer_body_is_valid(TSLexer *lexer) {
    uint8_t buf[ORG_LINE_BUF_MAX];
    for (;;) {
        if (lexer->eof(lexer)) return true;
        uint32_t len = 0;
        bool line_has_nul = false;
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            if (lexer->lookahead == 0) line_has_nul = true;
            if (len < ORG_LINE_BUF_MAX) buf[len++] = classify_byte(lexer->lookahead);
            lexer->advance(lexer, false);
        }
        if (!lexer->eof(lexer)) lexer->advance(lexer, false);  /* consume '\n' */
        PropdrawerLookahead v =
            prepass_propdrawer_lookahead(buf, len, line_has_nul);
        if (v == PROPDRAWER_LOOKAHEAD_DISQUALIFY) return false;
        if (v == PROPDRAWER_LOOKAHEAD_STOP) return true;
        /* PROPDRAWER_LOOKAHEAD_OK: keep scanning the next line. */
    }
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
    /* R4: set by any line-buffer read loop below that meets a raw
     * embedded NUL (never EOF - those loops already gate on eof()).
     * A NUL byte inside content destined for a JS-side regex token
     * (directive_value, property_value, block_args, ...) cannot be
     * tokenized: tree-sitter's generated lexer and its own
     * error-recovery use codepoint 0 as their EOF sentinel and never
     * advance past a real one, which livelocks the GLR parser.  Emacs
     * treats such bytes as opaque text, so once seen, the affected
     * line degrades to plain content instead of a structured token -
     * same "invalid shape falls back to paragraph" discipline used
     * everywhere else in this file. */
    bool saw_nul = false;

    /* Diary-sexp body: everything up to (not including) the line's
     * LAST `)` - Emacs reads to the outermost closing paren, so
     * nested parens stay in the body.  Empty bodies are refused
     * (grammar marks the field optional), which also keeps this
     * token from ever being zero-width.
     *
     * Guarded on the lookahead not already sitting at a line terminator:
     * like every external symbol, this one is also speculatively offered
     * during tree-sitter's own error-recovery "try anything" passes, at
     * positions with no diary-sexp body in play at all (e.g. a bare blank
     * line).  Entering there costs nothing extra to consume but still
     * hard-declines (`if (!marked) return false;` below) without ever
     * consuming a byte - starving `$._line_end` / `$._empty_line`, which
     * are external tokens now too, of a chance in THIS SAME call and
     * risking an error-recovery livelock hunting some other token. */
    if (valid_symbols[EXT_DIARY_SEXP_BODY]
        && lexer->lookahead != '\n' && lexer->lookahead != '\r'
        && !lexer->eof(lexer)) {
        if (lexer->lookahead != ')') {
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
            if (marked) {
                lexer->result_symbol = EXT_DIARY_SEXP_BODY;
                return true;
            }
            /* No closing paren before end-of-line/EOF: this candidate
             * body-read loop above already advanced the lexer up to the
             * terminator (whatever real bytes it read are not marked
             * into any token), so falling through here - rather than
             * hard-declining the whole call - lets Priority 2.5's
             * `$._line_end` (or Priority 5's `$._empty_line`) get a
             * genuine shot at that same, now-current position.  Same
             * reasoning as the body-text arm below. */
        }
    }

    /* -- Priority 0z: body-text token for comment_line / fixed_width_line.
     * Fires when the parser, having just consumed a prefix-only
     * `_comment_line` / `_fixed_width_line` token, asks for the body
     * field.  We read until end-of-line, marking_end after each non-
     * whitespace byte so trailing whitespace is excluded.
     *
     * Guarded on the lookahead already sitting at a terminator: this
     * symbol is also speculatively offered (like every external symbol)
     * during tree-sitter's own error-recovery "try anything" passes, at
     * positions with no real body-text field in play at all (e.g. a
     * bare blank line where `$._empty_line` is what should actually
     * fire).  Entering the block there and returning false unconditionally
     * would consume nothing but still deny every later priority (Priority
     * 2.5's `$._line_end`, Priority 5's `$._empty_line`) a chance in THIS
     * SAME call - since those are external tokens now too, not JS regexes
     * the generated lexer can fall back to on its own, that hard decline
     * starves them and error-recovery can livelock hunting for some other
     * token.  Skip the entire arm - not just the body-text symbol - when
     * nothing would be consumed anyway. */
    if ((valid_symbols[EXT_COMMENT_BODY_TEXT]
         || valid_symbols[EXT_FIXED_WIDTH_BODY_TEXT])
        && lexer->lookahead != '\n' && lexer->lookahead != '\r'
        && !lexer->eof(lexer)) {
        int sym = valid_symbols[EXT_COMMENT_BODY_TEXT]
                    ? EXT_COMMENT_BODY_TEXT : EXT_FIXED_WIDTH_BODY_TEXT;
        bool any_non_ws = false;
        /* Skip past leading whitespace, but include it in the token if
         * non-ws content follows (so `# foo` body is ` foo` - including
         * the leading space - which is the natural body slice). */
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && lexer->lookahead != '\r') {
            int32_t la = lexer->lookahead;
            lexer->advance(lexer, false);
            if (la != ' ' && la != '\t') {
                any_non_ws = true;
                lexer->mark_end(lexer);
            }
        }
        if (!any_non_ws) {
            if (valid_symbols[EXT_LINE_END] && !valid_symbols[EXT_EMPTY_LINE]
                && scan_line_end(lexer))
                return true;
            if (!valid_symbols[EXT_EMPTY_LINE]) return false;
            /* Deferring to `$._empty_line` (see the guard above) means
             * just that - fall all the way out of this arm, past the
             * `else` below, rather than returning false here.  The
             * body-read loop already advanced the lexer to the
             * terminator (an all-whitespace "empty" body), so Priority
             * 2.5 / Priority 5 get a genuine shot at that same position
             * in THIS call instead of a hard decline that starves them. */
        } else {
            lexer->result_symbol = (TSSymbol)sym;
            return true;
        }
    }

    /* -- Priority 0a: block_switches run after a src/example block's
     * language (or block-open, for example_block). See
     * scan_block_switches for why this is external rather than JS
     * regex tokens. */
    if (valid_symbols[EXT_BLOCK_SWITCHES]
        && (lexer->lookahead == '-' || lexer->lookahead == '+')) {
        if (scan_block_switches(lexer)) {
            lexer->result_symbol = EXT_BLOCK_SWITCHES;
            return true;
        }
    }

    /* -- Priority 0a: export-block backend name. See scan_export_format
     * for why this can't just reuse src_block_language's catch-all. */
    if (valid_symbols[EXT_EXPORT_FORMAT] && scan_export_format(lexer)) {
        lexer->result_symbol = EXT_EXPORT_FORMAT;
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
         * so editing those separators or this guard requires the other.
         *
         * Exception: `inlinetask_line` has no leading `/[ \t]+/` of its
         * own (unlike `headline_line`) - `_inlinetask_open` already
         * covers stars + exactly one whitespace byte, so `la` sitting
         * on whitespace here (only reachable when EXT_HEADLINE_TODO is
         * ALSO still a candidate, i.e. this is genuinely the first
         * headlineTail position) means a SECOND, un-owned whitespace
         * byte with no separator token to defer to. If nothing but
         * more whitespace remains before EOL/EOF, deferring can't find
         * a COMMENT word to classify either way and instead strands
         * that byte for tree-sitter's own error recovery - peek past
         * it (harmless: falling through to `return false` below still
         * restores the lexer here) and fold it into title directly
         * when there's truly nothing left to defer for. */
        if ((la == ' ' || la == '\t')
            && (valid_symbols[EXT_HEADLINE_COMMENT]
                || valid_symbols[EXT_HEADLINE_TODO])) {
            if (valid_symbols[EXT_HEADLINE_TODO]) {
                /* Fold straight into title rather than deferring: this
                 * un-owned byte has no leading separator token to
                 * defer to, and empirically the grammar state reached
                 * right after a redefining `_inlinetask_open` doesn't
                 * accept the internal `/[ \t]+/` separator token either
                 * (it only becomes reachable through tree-sitter's own
                 * error-recovery inserting a MISSING `_headline_todo`
                 * first) - deferring here either ERRORs outright or
                 * silently recovers into a malformed todo/title split.
                 * Always consuming it as title is an under-approximation
                 * (a genuine COMMENT/TODO word after the extra
                 * whitespace won't be classified as such) but is
                 * correct-by-construction: no ERROR either way. */
                int32_t prev = la;
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                    lexer->advance(lexer, false);
                if (valid_symbols[EXT_HEADLINE_TITLE]) {
                    lexer->mark_end(lexer);
                    scan_title_tail(lexer, prev, true);
                    lexer->result_symbol = EXT_HEADLINE_TITLE;
                    return true;
                }
            }
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
                        || lexer->eof(lexer)) {
                        lexer->mark_end(lexer);
                        lexer->result_symbol = EXT_HEADLINE_STATS_COOKIE;
                        return true;
                    }
                    if (lexer->lookahead == ':') {
                        /* Mark before probing so the tag validator's
                         * advances are outside the token; the next
                         * scan re-lexes the tags. */
                        lexer->mark_end(lexer);
                        int32_t last_consumed;
                        if (consume_tag_region(lexer, &last_consumed)) {
                            lexer->result_symbol = EXT_HEADLINE_STATS_COOKIE;
                            return true;
                        }
                        /* Failed tag region: reseed fallback_prev from
                         * the true last-consumed byte (not the stale
                         * pre-":" whitespace) so the title fallback
                         * below doesn't wrongly treat a later `[` as
                         * following whitespace it never actually
                         * followed - same fix as scan_title_tail's own
                         * `:` branch and consume_stats_cookie's nested
                         * probe. */
                        fallback_prev = last_consumed;
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

    /* -- Priority 2.5: line terminator (`[ \t]*\r?\n`, or zero-width at
     * EOF).  Must run before Priority 3's EOF short-circuit below - an
     * EOF-truncated line (table row end, drawer/block open, planning,
     * clock, ...) would otherwise hit that block's unconditional
     * `return false` before ever reaching this arm.  Must run after
     * every earlier content-specific arm above (body text, block
     * switches, headline sub-tokens, ...) so an optional body/value
     * field on the same line still gets first refusal on those bytes.
     * `_empty_line` wins the one contested position (a headline's
     * trailing newline is grammatically an `_empty_line`, see the
     * `headline` rule) - hence the `valid_symbols[EXT_EMPTY_LINE]`
     * exclusion.
     *
     * On failure this falls through rather than returning false:
     * `planning_line`'s `repeat1($.planning_entry)` makes a second
     * same-line `SCHEDULED:`/`DEADLINE:` keyword (EXT_PLANNING_LINE)
     * valid at the exact position a prior entry's line-end would be -
     * ws between entries makes scan_line_end advance then fail, and
     * Priority 5 below must still get a chance to detect the next
     * keyword.  Priority 5's classifiers skip leading whitespace
     * themselves (line_planning_clock_kind, is_drawer_line, ...), so
     * losing an already-consumed ws run to a failed probe here is
     * harmless - this mirrors how other zero-width closes in this
     * file (_propdrawer_close etc.) leave later arms to re-examine
     * the same bytes. */
    if (valid_symbols[EXT_LINE_END] && !valid_symbols[EXT_EMPTY_LINE]) {
        if (scan_line_end(lexer)) return true;
    }

    /* -- Priority 3: EOF - close all remaining open lists, then any
     * still-open containers (an unclosed block / drawer / inlinetask
     * runs to EOF), then headings. */
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
        /* A table row isn't tracked on the prepass scope stack (tables
         * aren't nesting containers), so close_innermost_scope never
         * sees it - an EOF-truncated row's last cell needs its own
         * zero-width close here. */
        if (valid_symbols[EXT_TABLE_ROW_END]) {
            lexer->result_symbol = EXT_TABLE_ROW_END;
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
    /* A `*` mid-row is opaque cell content (Emacs: any byte after `|` is
     * cell text) - table-cell/pipe/row-end being valid means we're
     * already inside a row, so skip the heading probe and let Priority 4a
     * consume it below.  Without this guard, a cell like `|* b` gets
     * partway into headline_line before the grammar discovers the row
     * doesn't fit that shape, with no token able to make progress. */
    bool mid_table_row = valid_symbols[EXT_TABLE_PIPE]
                       || valid_symbols[EXT_TABLE_CELL_CONTENT]
                       || valid_symbols[EXT_TABLE_ROW_END];
    if (lexer->lookahead == '*' && !mid_table_row) {
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
        } else if (level >= ORG_INLINETASK_MIN_LEVEL && at_line_start
                   && prepass_scope_top(s->prepass) == SCOPE_INLINETASK
                   && lexer->lookahead == ' ') {
            /* A second 15+-star line while an inlinetask is already open
             * either closes it (a real `org-inlinetask-END-regexp` match)
             * or redefines it (Emacs: any other such line starts a new,
             * sibling inlinetask) - peek past the mandatory space to tell
             * the two apart, mirroring is_inlinetask_end_line's lenience
             * (any horizontal whitespace run before/after the literal
             * `END`).
             *
             * Unlike the free-form peeks elsewhere in this file, this one
             * MUST end in a `return` on every path: mark_end is still at
             * line start (set above), which only stays the correct token
             * boundary as long as we either commit to it (`return true`,
             * next scan restarts fresh from there) or abandon the whole
             * candidate (`return false`, tree-sitter restores the pre-call
             * position) - falling through to the ordinary `consumed_stars`
             * path below would resume byte-buffering from wherever this
             * peek left the lexer instead of right after the stars. */
            lexer->advance(lexer, false);  /* the one mandatory space */
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                lexer->advance(lexer, false);
            bool is_close = false;
            if (lexer->lookahead == 'E') {
                lexer->advance(lexer, false);
                if (lexer->lookahead == 'N') {
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == 'D') {
                        lexer->advance(lexer, false);
                        while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                            lexer->advance(lexer, false);
                        is_close = lexer->eof(lexer)
                                 || lexer->lookahead == '\n'
                                 || lexer->lookahead == '\r';
                    }
                }
            }
            if (is_close) {
                /* `inlinetask: seq(inlinetask_line, ..., repeat(content_line),
                 * _inlinetask_close)` - by LR closure, EXT_INLINETASK_CLOSE
                 * is always a valid_symbols candidate anywhere inside an
                 * open inlinetask's body, so this guard should never
                 * actually trip on well-formed grammar states; it stays a
                 * `return false` (not a paragraph-fallback) rather than
                 * falling through, since the lexer is already advanced
                 * past the peeked END bytes and resuming the ordinary
                 * per-byte loop from here would corrupt line-buffering.
                 * A future change to the inlinetask production must keep
                 * this invariant or revisit this branch. */
                if (!valid_symbols[EXT_INLINETASK_CLOSE]) return false;
                if (lexer->lookahead == '\r') lexer->advance(lexer, false);
                if (lexer->lookahead == '\n') lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                prepass_scope_pop(s->prepass);
                lexer->result_symbol = EXT_INLINETASK_CLOSE;
                return true;
            }
            /* Same LR-closure invariant as above applies to
             * close_innermost_scope's own internal valid_symbols check
             * (it closes via this same EXT_INLINETASK_CLOSE symbol) - it
             * should never return false here either, and for the same
             * reason (lexer already past the peeked bytes) this can't
             * fall through to the ordinary path if it somehow did. */
            return close_innermost_scope(s, lexer, valid_symbols);
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
         * below keeps the line paragraph text.  Requires bullet_consumed
         * to be pure indentation (no failed-bullet byte, e.g. the `-` in
         * `-|`) - a real content byte before `|` disqualifies the line
         * as a table row (Emacs: `-|...` is a paragraph, not a row). */
        if (lexer->lookahead == '|'
            && bullet_consumed_len == indent
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
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && ll < ORG_LINE_BUF_MAX) {
            if (lexer->lookahead == 0) saw_nul = true;
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
                    if (dynblock_name_follows(lexer, line_buf2, &ll,
                                              ORG_LINE_BUF_MAX)) {
                        lexer->mark_end(lexer);
                        b2_mark = MARK_DYNBLOCK;
                        have_b2_mark = true;
                    }
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

        /* R4: b2_forced_sym is always CLOCK/PLANNING, both parsed by a
         * JS-side timestamp regex that cannot advance past a raw NUL -
         * degrade unconditionally (see the Priority 5 comment). */
        if (saw_nul && b2_forced_sym >= 0
            && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
        }

        if (b2_forced_sym >= 0) {
            if (b2_forced_sym == EXT_PLANNING_LINE
                && valid_symbols[EXT_PLANNING_LINE]
                && !planning_timestamp_follows(line_buf2, ll, b2_kw_colon)) {
                if (!valid_symbols[EXT_INLINE_CONTENT_LINE]) return false;
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            if (b2_forced_sym == EXT_CLOCK_LINE
                && valid_symbols[EXT_CLOCK_LINE]
                && !clock_line_is_valid(line_buf2, ll, b2_kw_colon)) {
                if (!valid_symbols[EXT_INLINE_CONTENT_LINE]) return false;
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            if (valid_symbols[b2_forced_sym]) {
                lexer->result_symbol = (TSSymbol)b2_forced_sym;
                return true;
            }
            /* Planning/clock are only valid directly under a headline
             * (planning) or wherever a content line can appear
             * (clock); anywhere else the line is plain paragraph
             * text rather than an unmapped-symbol hard failure. */
            if ((b2_forced_sym == EXT_PLANNING_LINE
                 || b2_forced_sym == EXT_CLOCK_LINE)
                && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            return false;
        }

        PrepassScopeSnapshot snap = prepass_scope_snapshot(s->prepass);
        LineClassification rr = prepass_classify_line(s->prepass, line_buf2, ll);

        /* E7: same whole-body look-ahead as Priority 5, for a
         * `:PROPERTIES:` open reached through this indented/bullet
         * fall-through path instead. */
        if (rr.type == TT_PROPDRAWER_OPEN && !propdrawer_body_is_valid(lexer)) {
            prepass_scope_restore(s->prepass, snap);
            prepass_scope_push(s->prepass, SCOPE_DRAWER);
            rr.type = TT_DRAWER_OPEN;
        }

        if (rr.type == TT_HEADING) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        if (saw_nul && rr.type != TT_BODY && rr.type != TT_EMPTY
            && rr.type != TT_COMMENT && rr.type != TT_FIXED_WIDTH
            && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
            prepass_scope_restore(s->prepass, snap);
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
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
        /* Plain body text needs the whole-line boundary regardless of
         * any earlier prefix mark (MARK_COLON, MARK_HASH, ...) - those
         * mark candidate structural prefixes (":", "#+", ...) before
         * the full line is known, and classification can still land on
         * TT_BODY despite one having fired (e.g. ":#+begin_s", whose
         * leading `:` isn't a drawer/property/fixed-width shape and
         * whose `#+begin_s` isn't a complete block keyword either).
         * Without this, the token stays pinned at the stale prefix
         * boundary and strands the rest of the line for a fresh,
         * out-of-context reclassification next scan. */
        if (rr.type == TT_BODY && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
        }
        int sym = prepass_to_external(rr.type);
        if (sym < 0) {
            prepass_scope_restore(s->prepass, snap);
            return false;
        }
        if (sym == EXT_KEYWORD_LINE && valid_symbols[EXT_FORMULA_LINE]
            && is_tblfm_directive(line_buf2, ll)) {
            sym = EXT_FORMULA_LINE;
        }
        if (rr.type == TT_EMPTY && valid_symbols[EXT_FN_EMPTY_LINE]
            && s->blank_run == 0) {
            sym = EXT_FN_EMPTY_LINE;
        }
        if (!valid_symbols[sym]) {
            /* No grammar slot for this classification here (e.g. a
             * `#+TBLFM:`/other keyword line indented inside a list item,
             * whose grammar only nests paragraph/list content) - same
             * graceful degradation as the table/lblock fallbacks above,
             * rather than a hard failure that would abort the whole scan
             * call at this position for every other candidate token. */
            prepass_scope_restore(s->prepass, snap);
            if (valid_symbols[EXT_INLINE_CONTENT_LINE]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
                return true;
            }
            return false;
        }
        lexer->result_symbol = (TSSymbol)sym;
        return true;
    }

    /* ── Priority 4d: footnote-def line — emit EXT_FOOTNOTE_DEF_LINE for
     * just `[fn:LABEL]`, leaving the rest of the line for
     * `_inline_content_line` to pick up.  Mirrors Emacs's
     * `org-element-footnote-definition-parser`, which treats anything
     * after `]` (including same-line text) as the body.
     *
     * Excluded inside a lesser block / latex environment: their bodies
     * are opaque external tokens with no sub-parsed children (same
     * reasoning as the blank-line body fix above), so a line there
     * must stay TT_LBLOCK_BODY / TT_LATEXENV_BODY regardless of shape.
     * Without this gate, `valid_symbols[EXT_FOOTNOTE_DEF_LINE]` can
     * still read true here during tree-sitter's blanket error-recovery
     * offering (every external is "valid" at every position then),
     * and this arm - reached before the scope-aware Priority 5
     * dispatch - would commit to a footnote-def read no grammar slot
     * actually has room for inside those two scopes, ERROR-ing. */
    if (consumed_stars == 0
        && valid_symbols[EXT_FOOTNOTE_DEF_LINE]
        && lexer->lookahead == '['
        && prepass_scope_top(s->prepass) != SCOPE_LBLOCK
        && prepass_scope_top(s->prepass) != SCOPE_LATEXENV) {
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
            if (lexer->lookahead == 0) saw_nul = true;
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
        if (saw_nul && fr.type != TT_BODY && fr.type != TT_EMPTY
            && fr.type != TT_COMMENT && fr.type != TT_FIXED_WIDTH
            && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
            prepass_scope_restore(s->prepass, snap);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
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
    while (!lexer->eof(lexer) && lexer->lookahead != '\n'
           && line_len < ORG_LINE_BUF_MAX) {
        if (lexer->lookahead == 0) saw_nul = true;
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
                if (dynblock_name_follows(lexer, line_buf, &line_len,
                                          ORG_LINE_BUF_MAX)) {
                    lexer->mark_end(lexer);
                    mark_kind = MARK_DYNBLOCK;
                    have_prefix_mark = true;
                }
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

        /* Keyword / affiliated keyword: `#+` prefix, optionally indented.
         * `ws_only_before` (not a bare `line_len == 2`) matches the
         * `lblock_name_offset`/MARK_COLON precedent a few lines below and
         * above: without it, this only fired when "#+" were literally
         * the line's first two bytes, so an indented generic keyword
         * (not "#+begin_"/"#+end_", which lblock_name_offset already
         * handles indent-tolerantly) fell through to the default
         * whole-line mark_end() - fine for a genuinely fresh, indented
         * top-level line (Priority 4c's list-open dispatch intercepts
         * those first and has its own indent-tolerant `#+` check), but
         * not for one reached via a SECOND scan mid-line (e.g. trailing
         * content after a lesser-block's `#+end_NAME` close, which is
         * never at column 0): there `get_column() == 0` is false, so
         * Priority 4c never fires and this was the only remaining `#+`
         * detector - the resulting whole-line-consuming external token
         * left nothing for the JS-side `directive_name`/`directive_value`
         * fields to match. */
        if (mark_kind == MARK_NONE
            && line_len >= 2 && line_buf[line_len - 1] == '+'
            && line_buf[line_len - 2] == '#'
            && ws_only_before(line_buf, line_len - 2)) {
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

    if (mark_kind == MARK_PLANNING && valid_symbols[EXT_PLANNING_LINE]
        && !planning_timestamp_follows(line_buf, line_len, p5_kw_colon)) {
        if (!valid_symbols[EXT_INLINE_CONTENT_LINE]) return false;
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
        return true;
    }
    if (mark_kind == MARK_CLOCK && valid_symbols[EXT_CLOCK_LINE]
        && !clock_line_is_valid(line_buf, line_len, p5_kw_colon)) {
        if (!valid_symbols[EXT_INLINE_CONTENT_LINE]) return false;
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
        return true;
    }

    PrepassScopeSnapshot snap = prepass_scope_snapshot(s->prepass);
    LineClassification r = prepass_classify_line(s->prepass, line_buf, line_len);

    /* E7: a `:PROPERTIES:` open only becomes `_propdrawer_open` when
     * every line through `:END:`/a headline/EOF is a safe node-property
     * line; otherwise redirect to the generic `_drawer_open` path
     * (drawer bodies already fall through to full line classification,
     * so tables/blocks/bad-shape lines inside just parse as plain
     * drawer content instead of livelocking or ERROR-ing). */
    if (r.type == TT_PROPDRAWER_OPEN && !propdrawer_body_is_valid(lexer)) {
        prepass_scope_restore(s->prepass, snap);
        prepass_scope_push(s->prepass, SCOPE_DRAWER);
        r.type = TT_DRAWER_OPEN;
    }

    if (r.type == TT_HEADING) {
        /* Should be unreachable: heading detection above handles col-0 '*'
         * lines. This branch fires only if the prepass disagrees (shouldn't
         * happen) — return false to avoid confusing the grammar. */
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    /* R4: everything except TT_BODY/TT_EMPTY (already plain content)
     * and TT_COMMENT/TT_FIXED_WIDTH (body already flows through the
     * NUL-safe external EXT_COMMENT_BODY_TEXT/EXT_FIXED_WIDTH_BODY_TEXT
     * path above) has a JS-side tail parsed by an internal regex token
     * that cannot advance past a raw NUL - degrade to plain content. */
    if (saw_nul && r.type != TT_BODY && r.type != TT_EMPTY
        && r.type != TT_COMMENT && r.type != TT_FIXED_WIDTH
        && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
        prepass_scope_restore(s->prepass, snap);
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
        return true;
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

    /* A bullet is only a list at a line's first non-whitespace position
     * (Priority 4c owns that case and runs before this classify).  Reaching
     * TT_LIST_ITEM here means either no grammar slot accepts a list right
     * now, or (footnote-def body reentry) this "line" is really a mid-line
     * suffix read from a lexer position that was never column 0 - Emacs
     * treats `[fn:1]+ x` as paragraph text, never a list.  Degrade to plain
     * content rather than emit an unmapped symbol, which would return
     * false and livelock GLR error recovery on the zero-width retry. */
    if (r.type == TT_LIST_ITEM) {
        if (valid_symbols[EXT_INLINE_CONTENT_LINE]) {
            lexer->mark_end(lexer);
            lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
            return true;
        }
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    /* Table lines with no table slot here stay paragraph text - the
     * list-item-continuation dispatch already has this exact fallback
     * (TT_TABLE_ROW/TT_TABLE_RULE deliberately aren't in
     * prepass_to_external's map, since a real table row is handled
     * cell-by-cell via dedicated Priority 4a/4b tokens instead), but
     * this column-0 dispatch never did: genuine top-level table content
     * always has EXT_TABLE_ROW_START offered via Priority 4b at column
     * 0, so the gap was unreachable until a SECOND, mid-line scan (not
     * at column 0, so Priority 4b's own `|`-at-lookahead check never
     * gets a turn) reads a table-row-shaped line with no table slot -
     * e.g. trailing `| a | b |` on a lesser-block's close line. Without
     * this, `sym < 0` below hard-fails since prepass_to_external has no
     * mapping for either type. */
    if ((r.type == TT_TABLE_ROW || r.type == TT_TABLE_RULE)
        && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
        lexer->mark_end(lexer);
        lexer->result_symbol = EXT_INLINE_CONTENT_LINE;
        return true;
    }

    /* Original r.type, before the CLOCK/PLANNING promotion below can
     * override `sym` away from EXT_INLINE_CONTENT_LINE - needed to
     * scope the whole-line mark_end() fix further down to genuine
     * plain-body lines only. */
    bool was_body = (r.type == TT_BODY);

    int sym = prepass_to_external(r.type);
    if (sym < 0) {
        prepass_scope_restore(s->prepass, snap);
        return false;
    }
    if (sym == EXT_KEYWORD_LINE && valid_symbols[EXT_FORMULA_LINE]
        && is_tblfm_directive(line_buf, line_len)) {
        sym = EXT_FORMULA_LINE;
    }

    /* Inside a footnote definition body an empty line is the gated
     * _fn_empty_line token; refusing it on the second consecutive
     * blank forces the footnote_definition to end. */
    if (r.type == TT_EMPTY && valid_symbols[EXT_FN_EMPTY_LINE]
        && s->blank_run == 0) {
        sym = EXT_FN_EMPTY_LINE;
    }

    /* Override the prepass classification when our in-line detection
     * recognised a CLOCK / planning entry. A scope whose classification
     * blankets non-close lines as TT_BODY (SCOPE_PROPDRAWER) would
     * otherwise lose CLOCK structure; this promotes it back when the
     * grammar has a slot for it (`valid_symbols[EXT_CLOCK_LINE]`).
     *
     * R4: gated on !saw_nul - this promotion is the one path where a
     * NUL-bearing line can reach here still classified TT_BODY (the
     * earlier saw_nul degrade deliberately excludes TT_BODY as
     * already-safe), and EXT_CLOCK_LINE/EXT_PLANNING_LINE's JS-side
     * timestamp regex is exactly the internal-token tail that
     * livelocks on a raw NUL. Suppressing the promotion leaves `sym`
     * at prepass_to_external(TT_BODY) == EXT_INLINE_CONTENT_LINE,
     * the same safe degradation used everywhere else in this file. */
    if (mark_kind == MARK_CLOCK && !saw_nul && valid_symbols[EXT_CLOCK_LINE]) {
        sym = EXT_CLOCK_LINE;
    } else if (mark_kind == MARK_PLANNING && !saw_nul
               && valid_symbols[EXT_PLANNING_LINE]) {
        sym = EXT_PLANNING_LINE;
    }
    if (sym == EXT_PLANNING_LINE && !valid_symbols[EXT_PLANNING_LINE]
        && valid_symbols[EXT_INLINE_CONTENT_LINE]) {
        lexer->mark_end(lexer);
        sym = EXT_INLINE_CONTENT_LINE;
    }
    /* Plain body text (never promoted above) needs the whole-line
     * boundary when MARK_COLON or MARK_COMMENT_LINE fired but
     * classification landed on TT_BODY anyway (a `:` line whose
     * remainder isn't drawer/property/fixed-width shaped; a `#` line
     * whose next byte isn't `+`/space/tab, e.g. "#*...", which the
     * mark's own eager `!= '+'` check accepts as a comment candidate
     * but the stricter classify_line shape check then rejects) - same
     * reasoning as the list-item-continuation dispatch's identical
     * fix. Scoped to these two specifically, NOT the broader "any
     * prefix mark": MARK_HASH/MARK_GBLOCK are deliberately excluded
     * from the "stop reconsidering" guard elsewhere in this file and
     * rely on staying short here (e.g. an unmatched `#+end_src`
     * splitting into its own token is what keeps "Headline terminates
     * src block" producing two separate paragraphs, not one merged
     * one - broadening this to MARK_HASH regressed that corpus test). */
    if (was_body
        && (mark_kind == MARK_COLON || mark_kind == MARK_COMMENT_LINE)
        && sym == EXT_INLINE_CONTENT_LINE) {
        lexer->mark_end(lexer);
    }
    /* MARK_LBLOCK (an "#+end_NAME" shaped prefix reclassified as plain
     * TT_BODY, e.g. an unmatched close outside any lesser-block scope)
     * is deliberately NOT in the blanket list above: "Headline
     * terminates src block" depends on a bare `#+end_src` with nothing
     * else on the line staying at its short mark (so it becomes its
     * own paragraph-starting token, splitting the following lines into
     * two separate paragraphs the way that corpus test expects).
     * Extend only when there's trailing non-whitespace content on the
     * SAME line past the recognized name (e.g. "#+end_example
     * #+TBLFM: ...") - there the short mark strands that content for a
     * fresh scan at a genuinely mid-line position, which has no valid
     * candidate to classify it as and ERRORs; a name running straight
     * to EOL never hits that starvation, so leaving it short is safe
     * AND required for the paragraph-split test above. */
    if (was_body && mark_kind == MARK_LBLOCK
        && sym == EXT_INLINE_CONTENT_LINE) {
        uint32_t i = 0;
        while (i < line_len && (line_buf[i] == ' ' || line_buf[i] == '\t')) i++;
        if (prefix_ci(line_buf + i, line_len - i, "#+end_")) {
            uint32_t j = i + 6;
            while (j < line_len && (
                (line_buf[j] >= 'a' && line_buf[j] <= 'z') ||
                (line_buf[j] >= 'A' && line_buf[j] <= 'Z') ||
                (line_buf[j] >= '0' && line_buf[j] <= '9') ||
                line_buf[j] == '_' || line_buf[j] == '-'
            )) j++;
            while (j < line_len && (line_buf[j] == ' ' || line_buf[j] == '\t')) j++;
            if (j < line_len) lexer->mark_end(lexer);
        }
    }
    /* TT_LBLOCK_BODY / TT_LATEXENV_BODY are opaque external tokens
     * with no JS-side tail (unlike TT_PLANNING/TT_CLOCK/TT_KEYWORD/...,
     * whose short prefix leaves the rest for JS or another external
     * token to consume) - they always want the whole-line boundary,
     * regardless of which prefix mark fired first. Safe to apply
     * unconditionally, including MARK_HASH: inside either scope,
     * classify_line's own scope dispatch runs before the general
     * `#+...` checks, so a `#+`-shaped line inside one of these
     * scopes is always TT_LBLOCK_BODY/TT_LATEXENV_BODY here, never
     * the plain TT_BODY case "Headline terminates src block" depends
     * on MARK_HASH staying short for. Fixes e.g. a MARK_PLANNING-
     * shaped `SCHEDULED:` line inside a latex environment: the early
     * MARK_PLANNING branch above only acts when EXT_PLANNING_LINE is
     * actually valid_symbols-offered, which it isn't inside these
     * scopes, so the mark it left behind otherwise strands here too. */
    if ((r.type == TT_LBLOCK_BODY || r.type == TT_LATEXENV_BODY)
        && valid_symbols[sym]) {
        lexer->mark_end(lexer);
    }
    if (!valid_symbols[sym]) {
        prepass_scope_restore(s->prepass, snap);
        return false;
    }

    lexer->result_symbol = (TSSymbol)sym;
    return true;
}
