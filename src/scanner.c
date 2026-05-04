#include "tree_sitter/parser.h"
#include "prepass.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
} ScannerState;

/* Match an ASCII-CI block name in `buf` of length `n` starting at
 * `start`.  Returns 1..5 for src/example/export/verse/comment, 0 if
 * unknown. */
static uint8_t lblock_kind_from(const uint8_t *buf, uint32_t start, uint32_t n) {
    /* Lower-case copy (up to 8 chars) to avoid case-sensitivity issues. */
    char low[8] = {0};
    uint32_t end = start + 8;
    if (end > n) end = n;
    for (uint32_t i = start, j = 0; i < end && j < 7; i++, j++) {
        uint8_t c = buf[i];
        low[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    if (strncmp(low, "src",     3) == 0) return 1;
    if (strncmp(low, "example", 7) == 0) return 2;
    if (strncmp(low, "export",  6) == 0) return 3;
    if (strncmp(low, "verse",   5) == 0) return 4;
    if (strncmp(low, "comment", 7) == 0) return 5;
    return 0;
}

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

    /* Binary layout (all fields present):
     *   [0]                                       : heading_depth
     *   [1 .. heading_depth]                      : heading_levels[]
     *   [1 + heading_depth]                       : pending_closes
     *   [2 + heading_depth]                       : pending_open_level
     *   [3 + heading_depth]                       : list_depth
     *   [4 + heading_depth .. list_depth bytes]   : list_indents[]
     *   [4 + heading_depth + list_depth]          : pending_list_closes
     *   [5 + heading_depth + list_depth ..]       : pending_list_open_indent (2 bytes, signed)
     *   [7 + heading_depth + list_depth ..]       : prepass serialized state
     */
    size_t hdr = 8u + (size_t)s->heading_depth + (size_t)s->list_depth;
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

    size_t pp_n = prepass_serialize(s->prepass, buf + pos, cap - pos);
    if (pp_n > cap - pos) return 0;
    return (unsigned)(pos + pp_n);
}

void tree_sitter_org_external_scanner_deserialize(void *payload,
                                                   const char *buffer,
                                                   unsigned length) {
    ScannerState    *s   = (ScannerState *)payload;
    const uint8_t   *buf = (const uint8_t *)buffer;
    if (length == 0) {
        s->pending_list_open_indent = -1;
        return;
    }

    size_t pos = 0;
    uint8_t hdepth = buf[pos++];
    if (hdepth > ORG_HEADING_STACK) return;
    if (length < (size_t)hdepth + 8) return;

    s->heading_depth = hdepth;
    if (hdepth > 0) {
        memcpy(s->heading_levels, buf + pos, hdepth);
        pos += hdepth;
    }
    s->pending_closes     = buf[pos++];
    s->pending_open_level = buf[pos++];

    uint8_t ldepth = buf[pos++];
    if (ldepth > ORG_LIST_STACK) return;
    if (length < pos + ldepth + 3) return;

    s->list_depth = ldepth;
    if (ldepth > 0) {
        memcpy(s->list_indents, buf + pos, ldepth);
        pos += ldepth;
    }
    s->pending_list_closes = buf[pos++];
    s->pending_list_open_indent =
        (int16_t)((uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8));
    pos += 2;
    s->lblock_kind = (length > pos) ? buf[pos++] : 0;

    if (length > pos)
        prepass_deserialize(s->prepass, buf + pos, (size_t)length - pos);
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
        return lexer->lookahead == '\n' || lexer->lookahead == 0
            || lexer->eof(lexer);
    }
}

/* Peek-test a statistics cookie at current position: `[N%]` or
 * `[N/M]`. Advances past the candidate; on return true the caller
 * should NOT mark_end past — keep the cookie outside the current
 * token's range. */
static bool consume_stats_cookie(TSLexer *lexer) {
    if (lexer->lookahead != '[') return false;
    lexer->advance(lexer, false);
    /* Optional digits before %/`/`. Empty `[/N]` and `[%]` are valid. */
    while (lexer->lookahead >= '0' && lexer->lookahead <= '9')
        lexer->advance(lexer, false);
    int32_t c = lexer->lookahead;
    if (c == '%') {
        lexer->advance(lexer, false);
        return lexer->lookahead == ']' && (lexer->advance(lexer, false), true);
    }
    if (c == '/') {
        lexer->advance(lexer, false);
        while (lexer->lookahead >= '0' && lexer->lookahead <= '9')
            lexer->advance(lexer, false);
        return lexer->lookahead == ']' && (lexer->advance(lexer, false), true);
    }
    return false;
}

/* Scan a headline title from the current position to end-of-line OR
 * the start of a trailing tag region OR a statistics cookie. The
 * title token covers everything UP TO the cookie / tag boundary
 * marker (`[` or `:` preceded by ws); the lexer rewinds to that
 * position so the next external scanner can fire on the marker
 * char directly. Trailing ws right before the boundary is included
 * in the title (Emacs convention; consumers may trim). */
static bool scan_headline_title(TSLexer *lexer) {
    bool any_title_chars = false;
    char prev = '\n';
    while (true) {
        int32_t c = lexer->lookahead;
        if (c == '\n' || c == 0 || lexer->eof(lexer)) break;
        if (c == ':' && (prev == ' ' || prev == '\t')) {
            if (consume_tag_region(lexer)) return any_title_chars;
            any_title_chars = true;
            lexer->mark_end(lexer);
            prev = (char)lexer->lookahead;
            continue;
        }
        if (c == '[' && (prev == ' ' || prev == '\t')) {
            if (consume_stats_cookie(lexer)) return any_title_chars;
            any_title_chars = true;
            lexer->mark_end(lexer);
            prev = (char)lexer->lookahead;
            continue;
        }
        lexer->advance(lexer, false);
        any_title_chars = true;
        lexer->mark_end(lexer);
        prev = (char)c;
    }
    return any_title_chars;
}

/* List checkbox: `[ ]` / `[x]` / `[X]` / `[-]` at the start of a
 * list_item's content, immediately after the bullet's whitespace. */
static bool scan_list_checkbox(TSLexer *lexer) {
    if (lexer->lookahead != '[') return false;
    lexer->advance(lexer, false);
    int32_t c = lexer->lookahead;
    bool ok = (c == ' ' || c == 'x' || c == 'X' || c == '-');
    if (!ok) return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != ']') return false;
    lexer->advance(lexer, false);
    /* Eat trailing inline whitespace so the paragraph's first
     * `_inline_content_line` token starts at the actual text. */
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
        lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    return true;
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

bool tree_sitter_org_external_scanner_scan(void *payload, TSLexer *lexer,
                                             const bool *valid_symbols) {
    ScannerState *s = (ScannerState *)payload;

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

    /* ── Priority 0a: list checkbox. Fires only when valid (i.e.
     * inside a list_item, immediately after the bullet's ws). */
    if (valid_symbols[EXT_LIST_CHECKBOX] && lexer->lookahead == '[') {
        if (scan_list_checkbox(lexer)) {
            lexer->result_symbol = EXT_LIST_CHECKBOX;
            return true;
        }
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
                    /* Consume trailing inline whitespace too, so the
                     * subsequent `tag_list` external scanner sees `:`
                     * directly (it requires `:` at lookahead). */
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
                        lexer->advance(lexer, false);
                    lexer->mark_end(lexer);
                    lexer->result_symbol = EXT_HEADLINE_STATS_COOKIE;
                    return true;
                }
                /* malformed — fall through to title fallback below */
            }
            /* Neither matched — `[...` is plain title content. We've
             * already advanced past `[`; continue title scan from here
             * and include those bytes in the title token. */
            if (!valid_symbols[EXT_HEADLINE_TITLE]) return false;
            lexer->mark_end(lexer);
            char prev = (char)lexer->lookahead;
            while (true) {
                int32_t c = lexer->lookahead;
                if (c == '\n' || c == 0 || lexer->eof(lexer)) break;
                if (c == ':' && (prev == ' ' || prev == '\t')) {
                    if (consume_tag_region(lexer)) {
                        lexer->result_symbol = EXT_HEADLINE_TITLE;
                        return true;
                    }
                    lexer->mark_end(lexer);
                    prev = (char)lexer->lookahead;
                    continue;
                }
                if (c == '[' && (prev == ' ' || prev == '\t')) {
                    if (consume_stats_cookie(lexer)) {
                        lexer->result_symbol = EXT_HEADLINE_TITLE;
                        return true;
                    }
                    lexer->mark_end(lexer);
                    prev = (char)lexer->lookahead;
                    continue;
                }
                lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                prev = (char)c;
            }
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
            char prev = (char)lexer->lookahead;
            while (true) {
                int32_t c = lexer->lookahead;
                if (c == '\n' || c == 0 || lexer->eof(lexer)) break;
                if (c == ':' && (prev == ' ' || prev == '\t')) {
                    if (consume_tag_region(lexer)) {
                        lexer->result_symbol = EXT_HEADLINE_TITLE;
                        return true;
                    }
                    lexer->mark_end(lexer);
                    prev = (char)lexer->lookahead;
                    continue;
                }
                if (c == '[' && (prev == ' ' || prev == '\t')) {
                    if (consume_stats_cookie(lexer)) {
                        lexer->result_symbol = EXT_HEADLINE_TITLE;
                        return true;
                    }
                    lexer->mark_end(lexer);
                    prev = (char)lexer->lookahead;
                    continue;
                }
                lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                prev = (char)c;
            }
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
        s->heading_levels[s->heading_depth++] = level;
        lexer->result_symbol = EXT_HEADING_OPEN;
        return true;
    }

    /* ── Priority 3: EOF — close all remaining open lists, then headings. ── */
    if (lexer->eof(lexer)) {
        if (s->list_depth > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
            lexer->mark_end(lexer);
            s->list_depth--;
            lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
            return true;
        }
        if (s->heading_depth > 0 && valid_symbols[EXT_HEADING_CLOSE]) {
            lexer->mark_end(lexer);
            s->heading_depth--;
            lexer->result_symbol = EXT_HEADING_CLOSE;
            return true;
        }
        return false;
    }

    /* ── Priority 3a: passive list close.  We're inside a list and the
     * parser will accept a close.  Fire when:
     *
     *   - lookahead is at column 0 (start of a fresh line), AND
     *   - lookahead is NOT a bullet candidate, AND
     *   - lookahead is NOT whitespace (which would make the indent > 0).
     *
     * Per Emacs, paragraph continuation inside a list item requires the
     * line to be indented strictly more than the item's bullet column.
     * If column 0 indent is ≤ any open list's indent (always, since
     * indents are ≥ 0), the list must end before this line is parsed. */
    if (s->list_depth > 0
        && valid_symbols[EXT_PLAIN_LIST_CLOSE]
        && lexer->get_column(lexer) == 0) {
        uint8_t la = (uint8_t)lexer->lookahead;
        /* `*` at column 0 is never a list bullet (the prepass only treats
         * `*` as a bullet when indent > 0).  It's either a heading or
         * inline emphasis, and either way the list ends. */
        bool maybe_bullet = (la == ' ' || la == '\t'
                              || la == '-' || la == '+'
                              || (la >= '0' && la <= '9'));
        if (!maybe_bullet) {
            lexer->mark_end(lexer);
            s->list_depth--;
            lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
            return true;
        }
    }

    /* ── Priority 4: detect heading line; emit close(s) then open. ───── */
    uint8_t consumed_stars = 0;
    if (lexer->lookahead == '*') {
        /* Call mark_end before advancing, so that if we emit a
         * _heading_close the token is zero-width and the next scan()
         * call will re-present the same line for the _heading_open. */
        lexer->mark_end(lexer);

        /* Count leading stars. */
        uint8_t level = 0;
        while (!lexer->eof(lexer) && lexer->lookahead == '*') {
            level++;
            lexer->advance(lexer, false);
        }

        bool is_heading = (level > 0 && level < 15
                           && !lexer->eof(lexer)
                           && lexer->lookahead == ' ');

        if (is_heading) {
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

        if (lexer->lookahead == '\n' && valid_symbols[EXT_TABLE_ROW_END]) {
            lexer->advance(lexer, false);
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
            && !lexer->eof(lexer)) {
            while (!lexer->eof(lexer)
                   && lexer->lookahead != '|'
                   && lexer->lookahead != '\n') {
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

        static uint8_t row_buf[ORG_LINE_BUF_MAX];
        uint32_t row_len = 1;
        row_buf[0] = '|';
        while (!lexer->eof(lexer)
               && lexer->lookahead != '\n'
               && row_len < ORG_LINE_BUF_MAX) {
            row_buf[row_len++] = (uint8_t)lexer->lookahead;
            lexer->advance(lexer, false);
        }

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
        return false;
    }

    /* ── Priority 4c: list-item bullet detection.  Emits a token covering
     * exactly the indent + bullet chars + trailing whitespace.  Supports
     * `- `, `+ `, `* ` (only at indent > 0), and `N.` / `N)` numeric
     * bullets.  `[@N]` counters and `[X]` checkboxes are not split out
     * inline yet — they'd require advancing further; for now the
     * normalizer's wrap_item_content covers items that aren't matched
     * here. */
    if (consumed_stars == 0
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
        while ((lexer->lookahead == ' ' || lexer->lookahead == '\t')
               && bullet_consumed_len < 64) {
            bullet_consumed[bullet_consumed_len++] = (uint8_t)lexer->lookahead;
            lexer->advance(lexer, false);
            indent++;
        }

        bool ok = false;
        uint8_t la = (uint8_t)lexer->lookahead;

        if (la == '-' || la == '+' || (la == '*' && indent > 0)) {
            bullet_consumed[bullet_consumed_len++] = la;
            lexer->advance(lexer, false);
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                bullet_consumed[bullet_consumed_len++] = (uint8_t)lexer->lookahead;
                lexer->advance(lexer, false);
                ok = true;
            }
        } else if (la >= '0' && la <= '9') {
            while (lexer->lookahead >= '0' && lexer->lookahead <= '9'
                   && bullet_consumed_len < 64) {
                bullet_consumed[bullet_consumed_len++] = (uint8_t)lexer->lookahead;
                lexer->advance(lexer, false);
            }
            if (lexer->lookahead == '.' || lexer->lookahead == ')') {
                bullet_consumed[bullet_consumed_len++] = (uint8_t)lexer->lookahead;
                lexer->advance(lexer, false);
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    bullet_consumed[bullet_consumed_len++] = (uint8_t)lexer->lookahead;
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

        /* Not a real bullet.  Decide whether the line is a paragraph
         * continuation of the current list item or a list-terminating
         * line.  The Emacs rule: a continuation line must be indented
         * strictly more than the item's bullet column (== the entry on
         * top of the list stack).  If the indent is equal or less, the
         * list ends here. */
        if (s->list_depth > 0 && valid_symbols[EXT_PLAIN_LIST_CLOSE]) {
            uint8_t top = s->list_indents[s->list_depth - 1];
            if ((uint32_t)indent <= (uint32_t)top) {
                s->list_depth--;
                lexer->result_symbol = EXT_PLAIN_LIST_CLOSE;
                return true;
            }
            /* indent > top → paragraph continuation; fall through to line
             * classify (the consumed indent bytes are prepended below). */
        }

        /* No list open and not a bullet — fall through to Priority 5 with
         * the consumed bytes prepended so prepass sees the full line.
         * Apply the same MARK_COLON shortcut here so an indented
         * `  :PROPERTIES:` / `  :KEY: value` exposes the leading `:`
         * as a separate token. */
        static uint8_t line_buf2[ORG_LINE_BUF_MAX];
        uint32_t ll = 0;
        for (uint32_t i = 0; i < bullet_consumed_len && ll < ORG_LINE_BUF_MAX; i++)
            line_buf2[ll++] = bullet_consumed[i];
        /* Pre-mark at start so a zero-width emit (planning / clock) is
         * possible.  Mid-loop colon-mark moves it forward; if planning/
         * clock is detected, we leave mark_end at start. */
        bool have_b2_mark = false;
        int  b2_forced_sym = -1;  /* >=0 = override prepass with this symbol */
        lexer->mark_end(lexer);
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && ll < ORG_LINE_BUF_MAX) {
            line_buf2[ll++] = (uint8_t)lexer->lookahead;
            lexer->advance(lexer, false);
            if (have_b2_mark) continue;
            if (line_buf2[ll - 1] == ':') {
                bool ws_only = true;
                for (uint32_t i = 0; i + 1 < ll; i++) {
                    if (line_buf2[i] != ' ' && line_buf2[i] != '\t') {
                        ws_only = false; break;
                    }
                }
                /* `:` after only whitespace → drawer/property OR
                 * fixed-width prefix.  Mark at `:` so the emitted
                 * token covers only the colon; JS rules consume the
                 * rest (name + closing `:`, or body text). */
                if (ws_only) {
                    lexer->mark_end(lexer);
                    have_b2_mark = true;
                    continue;
                }
                /* Planning / clock keyword (`SCHEDULED:` / `DEADLINE:`
                 * / `CLOSED:` / `CLOCK:`) at the start of the trimmed
                 * content. Emit zero-width so JS rules consume the
                 * keyword + timestamp(s). */
                if (ll >= 6) {
                    uint32_t i = 0;
                    while (i < ll && (line_buf2[i] == ' ' || line_buf2[i] == '\t')) i++;
                    uint32_t klen = (ll - 1) - i;
                    const uint8_t *p = line_buf2 + i;
                    bool match = false;
                    if (klen == 9 && p[0]=='S' && p[1]=='C' && p[2]=='H' && p[3]=='E'
                        && p[4]=='D' && p[5]=='U' && p[6]=='L' && p[7]=='E' && p[8]=='D') match = true;
                    else if (klen == 8 && p[0]=='D' && p[1]=='E' && p[2]=='A' && p[3]=='D'
                        && p[4]=='L' && p[5]=='I' && p[6]=='N' && p[7]=='E') match = true;
                    else if (klen == 6 && p[0]=='C' && p[1]=='L' && p[2]=='O' && p[3]=='S'
                        && p[4]=='E' && p[5]=='D') match = true;
                    else if (klen == 5 && p[0]=='C' && p[1]=='L' && p[2]=='O' && p[3]=='C'
                        && p[4]=='K') match = true;
                    if (match) {
                        if (klen == 5) {
                            /* CLOCK: emit prefix-covering token (up to
                             * and including the `:`). Avoids zero-
                             * width which causes error-recovery loops
                             * inside unclosed drawers. */
                            lexer->mark_end(lexer);
                            b2_forced_sym = EXT_CLOCK_LINE;
                        } else {
                            /* Planning: zero-width (mark stays at
                             * line start) — JS rule consumes keyword
                             * + timestamp pairs. */
                            b2_forced_sym = EXT_PLANNING_LINE;
                        }
                        have_b2_mark = true;
                        continue;
                    }
                }
            }
        }
        if (!lexer->eof(lexer) && lexer->lookahead == '\n')
            lexer->advance(lexer, false);
        if (!have_b2_mark) lexer->mark_end(lexer);

        /* Forced-symbol path takes precedence over the prepass
         * classification (which may not recognise indented CLOCK /
         * SCHEDULED lines as TT_CLOCK / TT_PLANNING). */
        if (b2_forced_sym >= 0) {
            if (!valid_symbols[b2_forced_sym]) return false;
            lexer->result_symbol = (TSSymbol)b2_forced_sym;
            return true;
        }

        LineClassification rr = prepass_classify_line(s->prepass, line_buf2, ll);
        if (rr.type == TT_HEADING) return false;
        int sym = prepass_to_external(rr.type);
        if (sym < 0) return false;
        if (!valid_symbols[sym]) return false;
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
        static uint8_t fn_line_buf[ORG_LINE_BUF_MAX];
        uint32_t fn_ll = 0;
        for (uint32_t i = 0; i < fn_len && fn_ll < ORG_LINE_BUF_MAX; i++)
            fn_line_buf[fn_ll++] = fn_consumed[i];
        while (!lexer->eof(lexer) && lexer->lookahead != '\n'
               && fn_ll < ORG_LINE_BUF_MAX) {
            fn_line_buf[fn_ll++] = (uint8_t)lexer->lookahead;
            lexer->advance(lexer, false);
        }
        if (!lexer->eof(lexer) && lexer->lookahead == '\n')
            lexer->advance(lexer, false);
        lexer->mark_end(lexer);

        LineClassification fr = prepass_classify_line(s->prepass, fn_line_buf, fn_ll);
        if (fr.type == TT_HEADING) return false;
        int fsym = prepass_to_external(fr.type);
        if (fsym < 0) return false;
        if (!valid_symbols[fsym]) return false;
        lexer->result_symbol = (TSSymbol)fsym;
        return true;
    }

    /* ── Priority 5: classify line via prepass (non-heading). ────────── */
    static uint8_t line_buf[ORG_LINE_BUF_MAX];
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
    enum { MARK_NONE, MARK_HASH, MARK_LBLOCK, MARK_COLON,
           MARK_DRAWER, MARK_PROPERTY, MARK_PLANNING, MARK_CLOCK,
           MARK_INLINETASK, MARK_DIARY_SEXP, MARK_COMMENT_LINE };
    int mark_kind = MARK_NONE;
    bool have_prefix_mark = false;
    static const uint32_t name_len_for_kind[] = {0, 3, 7, 6, 5, 7};
    /* Pre-mark at line start so a zero-width emit (planning / clock)
     * is possible. mid-loop mark_end calls move this forward; if
     * planning/clock is detected, we leave mark_end untouched. */
    lexer->mark_end(lexer);
    while (!lexer->eof(lexer) && lexer->lookahead != '\n'
           && line_len < ORG_LINE_BUF_MAX) {
        line_buf[line_len++] = (uint8_t)lexer->lookahead;
        lexer->advance(lexer, false);

        /* `:`-leading line (after optional leading whitespace).  Mark
         * after the first `:` so the eventual `_drawer_open` /
         * `_node_property_line` / `_fixed_width_line` token covers
         * only that single colon; JS rules then consume the rest
         * (name + closing `:` + value, or body text). */
        if (mark_kind == MARK_NONE && line_buf[line_len - 1] == ':') {
            /* Verify all preceding chars in the line so far are
             * whitespace — i.e., this is the FIRST non-ws char. */
            bool ws_only = true;
            for (uint32_t i = 0; i + 1 < line_len; i++) {
                if (line_buf[i] != ' ' && line_buf[i] != '\t') {
                    ws_only = false; break;
                }
            }
            if (ws_only) {
                lexer->mark_end(lexer);
                mark_kind = MARK_COLON;
                have_prefix_mark = true;
                continue;
            }
        }

        /* Lblock open is the most specific `#+` form — always allow
         * upgrade from MARK_HASH to MARK_LBLOCK. */
        if (mark_kind != MARK_LBLOCK
            && mark_kind != MARK_DRAWER
            && mark_kind != MARK_PROPERTY) {
            uint32_t off = lblock_name_offset(line_buf, line_len);
            if (off > 0) {
                uint8_t kind = lblock_kind_from(line_buf, off, line_len);
                if (kind > 0 && kind <= 5) {
                    uint32_t prefix_end = off + name_len_for_kind[kind];
                    if (line_len == prefix_end) {
                        int32_t la2 = lexer->lookahead;
                        if (la2 == ' ' || la2 == '\t' || la2 == '\n'
                            || la2 == 0 || lexer->eof(lexer)) {
                            lexer->mark_end(lexer);
                            mark_kind = MARK_LBLOCK;
                            have_prefix_mark = true;
                            continue;
                        }
                    }
                }
            }
        }

        if (mark_kind == MARK_LBLOCK
            || mark_kind == MARK_DRAWER
            || mark_kind == MARK_PROPERTY
            || mark_kind == MARK_PLANNING
            || mark_kind == MARK_CLOCK
            || mark_kind == MARK_INLINETASK
            || mark_kind == MARK_DIARY_SEXP
            || mark_kind == MARK_COMMENT_LINE) continue;

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
            && line_buf[line_len - 1] == ' ' && line_len >= 16) {
            uint32_t star_count = 0;
            while (star_count < line_len - 1
                   && line_buf[star_count] == '*') star_count++;
            if (star_count >= 15 && star_count == line_len - 1) {
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
         * or clock line (`CLOCK:`). Emit a ZERO-WIDTH token at line
         * start so JS rules consume keyword + `:` + ws + timestamp. */
        if (mark_kind == MARK_NONE
            && line_buf[line_len - 1] == ':' && line_len >= 6) {
            uint32_t i = 0;
            while (i < line_len && (line_buf[i] == ' ' || line_buf[i] == '\t')) i++;
            uint32_t klen = (line_len - 1) - i;
            const uint8_t *p = line_buf + i;
            bool match = false;
            int new_kind = MARK_NONE;
            if (klen == 9 && p[0]=='S' && p[1]=='C' && p[2]=='H' && p[3]=='E'
                && p[4]=='D' && p[5]=='U' && p[6]=='L' && p[7]=='E' && p[8]=='D') {
                match = true; new_kind = MARK_PLANNING;
            } else if (klen == 8 && p[0]=='D' && p[1]=='E' && p[2]=='A' && p[3]=='D'
                && p[4]=='L' && p[5]=='I' && p[6]=='N' && p[7]=='E') {
                match = true; new_kind = MARK_PLANNING;
            } else if (klen == 6 && p[0]=='C' && p[1]=='L' && p[2]=='O' && p[3]=='S'
                && p[4]=='E' && p[5]=='D') {
                match = true; new_kind = MARK_PLANNING;
            }
            else if (klen == 5 && p[0]=='C' && p[1]=='L' && p[2]=='O' && p[3]=='C'
                && p[4]=='K') {
                match = true; new_kind = MARK_CLOCK;
            }
            if (match) {
                if (new_kind == MARK_CLOCK) {
                    /* Mark covers up to and including the `:`. Non-
                     * zero-width to avoid tree-sitter error-recovery
                     * loops inside unclosed drawers. */
                    lexer->mark_end(lexer);
                }
                /* MARK_PLANNING stays zero-width (mark from line-
                 * start pre-mark). JS rule consumes keyword +
                 * timestamp pairs. */
                mark_kind = new_kind;
                have_prefix_mark = true;
                continue;
            }
        }

    }
    /* Consume the trailing newline (if any). */
    if (!lexer->eof(lexer) && lexer->lookahead == '\n')
        lexer->advance(lexer, false);

    /* Default token end = current position (whole line consumed).
     * If we DID set a prefix mark, that one stays in effect for an
     * lblock-open emit. */
    if (!have_prefix_mark) lexer->mark_end(lexer);

    LineClassification r = prepass_classify_line(s->prepass, line_buf, line_len);

    if (r.type == TT_HEADING) {
        /* Should be unreachable: heading detection above handles col-0 '*'
         * lines. This branch fires only if the prepass disagrees (shouldn't
         * happen) — return false to avoid confusing the grammar. */
        return false;
    }

    /* Lesser-block dispatch: emit one of 5 type-specific tokens based on
     * the block name.  TT_LBLOCK_OPEN sets `lblock_kind`; TT_LBLOCK_CLOSE
     * dispatches by that saved kind and clears it. */
    if (r.type == TT_LBLOCK_OPEN) {
        uint32_t off = lblock_name_offset(line_buf, line_len);
        uint8_t kind = off ? lblock_kind_from(line_buf, off, line_len) : 0;
        s->lblock_kind = kind;
        int open_sym = -1;
        switch (kind) {
            case 1: open_sym = EXT_SRC_BLOCK_OPEN;     break;
            case 2: open_sym = EXT_EXAMPLE_BLOCK_OPEN; break;
            case 3: open_sym = EXT_EXPORT_BLOCK_OPEN;  break;
            case 4: open_sym = EXT_VERSE_BLOCK_OPEN;   break;
            case 5: open_sym = EXT_COMMENT_BLOCK_OPEN; break;
            default: return false;
        }
        if (!valid_symbols[open_sym]) return false;
        /* If the prefix-mark wasn't set during the read (e.g.
         * line_len overflowed buf), fall back to the whole-line
         * mark_end (which `lexer->mark_end(lexer)` above set). */
        lexer->result_symbol = (TSSymbol)open_sym;
        return true;
    }
    if (r.type == TT_LBLOCK_CLOSE) {
        uint8_t kind = s->lblock_kind;
        s->lblock_kind = 0;
        int close_sym = -1;
        switch (kind) {
            case 1: close_sym = EXT_SRC_BLOCK_CLOSE;     break;
            case 2: close_sym = EXT_EXAMPLE_BLOCK_CLOSE; break;
            case 3: close_sym = EXT_EXPORT_BLOCK_CLOSE;  break;
            case 4: close_sym = EXT_VERSE_BLOCK_CLOSE;   break;
            case 5: close_sym = EXT_COMMENT_BLOCK_CLOSE; break;
            default: return false;
        }
        if (!valid_symbols[close_sym]) return false;
        lexer->result_symbol = (TSSymbol)close_sym;
        return true;
    }

    int sym = prepass_to_external(r.type);
    if (sym < 0) return false;

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
    if (!valid_symbols[sym]) return false;

    lexer->result_symbol = (TSSymbol)sym;
    return true;
}
