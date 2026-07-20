#include "prepass.h"

#include <stdlib.h>
#include <string.h>

#define PREPASS_STACK_MAX 32

struct prepass_state {
    uint8_t           stack[PREPASS_STACK_MAX];
    uint8_t           depth;
};

static void scope_push(struct prepass_state *s, ScopeKind kind) {
    if (s->depth < PREPASS_STACK_MAX) s->stack[s->depth++] = (uint8_t)kind;
}

static ScopeKind scope_top(const struct prepass_state *s) {
    return s->depth ? (ScopeKind)s->stack[s->depth - 1] : SCOPE_NONE;
}

static void scope_pop(struct prepass_state *s) {
    if (s->depth) s->depth--;
}

ScopeKind prepass_scope_top(const prepass_state_t *s) {
    return scope_top((const struct prepass_state *)s);
}

void prepass_scope_pop(prepass_state_t *s) {
    scope_pop((struct prepass_state *)s);
}

void prepass_scope_push(prepass_state_t *s, ScopeKind kind) {
    scope_push((struct prepass_state *)s, kind);
}

PrepassScopeSnapshot prepass_scope_snapshot(const prepass_state_t *s) {
    const struct prepass_state *st = (const struct prepass_state *)s;
    PrepassScopeSnapshot snap;
    snap.depth = st->depth;
    snap.top = st->depth ? st->stack[st->depth - 1] : 0;
    return snap;
}

void prepass_scope_restore(prepass_state_t *s, PrepassScopeSnapshot snap) {
    struct prepass_state *st = (struct prepass_state *)s;
    st->depth = snap.depth;
    if (snap.depth) st->stack[snap.depth - 1] = snap.top;
}

/* Forward declarations for classify_line and leading_indent. */
static LineTokenType classify_line(struct prepass_state *s,
                                   const uint8_t *line, uint32_t line_len,
                                   uint16_t indent, uint64_t *out_meta);
extern uint16_t organ_leading_indent_scalar(const uint8_t *p, uint32_t len);
#if ORGAN_PREPASS_USE_SIMD
extern uint16_t organ_leading_indent_swar(const uint8_t *p, uint32_t len);
#define leading_indent organ_leading_indent_swar
#else
#define leading_indent organ_leading_indent_scalar
#endif

prepass_state_t *prepass_state_new(void) {
    struct prepass_state *s = (struct prepass_state *)calloc(1, sizeof(struct prepass_state));
    if (!s) return NULL;
    s->depth = 0;
    return (prepass_state_t *)s;
}

void prepass_state_free(prepass_state_t *s) {
    if (!s) return;
    free(s);
}

LineClassification prepass_classify_line(prepass_state_t *s,
                                          const uint8_t *line,
                                          uint32_t line_len) {
    struct prepass_state *st = (struct prepass_state *)s;
    uint16_t indent = leading_indent(line, line_len);
    uint64_t meta = 0;
    uint8_t  depth_before = st->depth;
    LineTokenType type = classify_line(st, line, line_len, indent, &meta);
    LineClassification r = {
        .type               = type,
        .indent_col         = indent,
        .stack_depth_before = depth_before,
        .meta               = meta,
    };
    return r;
}

void prepass_reset(prepass_state_t *s) {
    struct prepass_state *st = (struct prepass_state *)s;
    st->depth = 0;
}

/* --- hot helpers (whitespace skip, drawer-name scan, block-name scan) --- */

static int is_drawer_line(const uint8_t *p, uint32_t rem,
                          uint32_t *name_start, uint32_t *name_end) {
    /* Emacs `org-drawer-regexp` is `^[ \t]*:\\(\\([-_[:word:]]+\\)\\):
     * [ \t]*$` — name allows letters of either case, digits, underscore,
     * and hyphen.  We're permissive on character class to match. */
    if (rem < 3 || p[0] != ':') return 0;
    uint32_t i = 1;
    while (i < rem && (
        (p[i] >= 'A' && p[i] <= 'Z') ||
        (p[i] >= 'a' && p[i] <= 'z') ||
        (p[i] >= '0' && p[i] <= '9') ||
        p[i] == '_' || p[i] == '-'
    )) i++;
    if (i == 1 || i >= rem || p[i] != ':') return 0;
    uint32_t j = i + 1;
    while (j < rem && (p[j] == ' ' || p[j] == '\t')) j++;
    if (j != rem) return 0;
    *name_start = 1;
    *name_end = i;
    return 1;
}

static int parse_block_name(const uint8_t *trimmed, uint32_t rem,
                            uint32_t name_offset,
                            uint32_t *name_start, uint32_t *name_end) {
    uint32_t i = name_offset;
    while (i < rem && (
        (trimmed[i] >= 'a' && trimmed[i] <= 'z') ||
        (trimmed[i] >= 'A' && trimmed[i] <= 'Z') ||
        (trimmed[i] >= '0' && trimmed[i] <= '9') ||
        trimmed[i] == '_' || trimmed[i] == '-'
    )) i++;
    if (i == name_offset) return 0;
    *name_start = name_offset;
    *name_end = i;
    return 1;
}

/* --- end hot helpers --- */

static int has_prefix_ci(const uint8_t *p, uint32_t len, const char *kw) {
    size_t klen = strlen(kw);
    if (len < klen) return 0;
    for (size_t i = 0; i < klen; i++) {
        uint8_t a = p[i];
        uint8_t b = (uint8_t)kw[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return 0;
    }
    return 1;
}

static int is_planning(const uint8_t *p, uint32_t rem) {
    static const char *KWS[] = { "scheduled:", "deadline:", "closed:" };
    for (int i = 0; i < 3; i++) {
        if (has_prefix_ci(p, rem, KWS[i])) return 1;
    }
    return 0;
}

/* ASCII case-insensitive equals.  Emacs treats drawer keywords
 * (`:PROPERTIES:` / `:END:` / etc.) case-insensitively per
 * `org-drawer-regexp` + the surrounding `case-fold-search = t`. */
static int name_iequals(const uint8_t *p, uint32_t start, uint32_t end,
                        const char *kw) {
    size_t klen = strlen(kw);
    if (end - start != klen) return 0;
    for (size_t i = 0; i < klen; i++) {
        uint8_t a = p[start + i];
        uint8_t b = (uint8_t)kw[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int is_node_property(const uint8_t *p, uint32_t rem) {
    /* Emacs `org-property-re` accepts `[-_[:alnum:]]+` for the key,
     * so both upper- and lowercase letters are valid.  The trailing
     * `+` enables the `KEY+:` value-append syntax. */
    if (rem < 4 || p[0] != ':') return 0;
    uint32_t i = 1;
    while (i < rem && (
        (p[i] >= 'A' && p[i] <= 'Z') ||
        (p[i] >= 'a' && p[i] <= 'z') ||
        (p[i] >= '0' && p[i] <= '9') ||
        p[i] == '_' || p[i] == '-' || p[i] == '+'
    )) i++;
    if (i == 1 || i >= rem || p[i] != ':') return 0;
    return 1;
}

static int name_eq_ci(const uint8_t *p, uint32_t start, uint32_t end,
                      const char *kw) {
    size_t klen = strlen(kw);
    if (end - start != klen) return 0;
    for (size_t i = 0; i < klen; i++) {
        uint8_t a = p[start + i];
        uint8_t b = (uint8_t)kw[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return 0;
    }
    return 1;
}

/* Returns 1 if `p[s..e)` names an affiliated keyword.  Affiliated keywords
 * (Emacs `org-element-affiliated-keywords` plus `ATTR_*`) attach to the
 * following element rather than standing on their own. */
static int is_affiliated_keyword_name(const uint8_t *p, uint32_t s, uint32_t e) {
    if (name_eq_ci(p, s, e, "CAPTION")) return 1;
    if (name_eq_ci(p, s, e, "NAME"))    return 1;
    if (name_eq_ci(p, s, e, "RESULTS")) return 1;
    if (name_eq_ci(p, s, e, "HEADER"))  return 1;
    if (name_eq_ci(p, s, e, "HEADERS")) return 1;
    if (name_eq_ci(p, s, e, "PLOT"))    return 1;
    /* ATTR_* prefix (case-insensitive). */
    if (e - s >= 5) {
        char low[5] = {0};
        for (uint32_t i = 0; i < 5; i++) {
            uint8_t c = p[s + i];
            low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
        }
        if (low[0] == 'a' && low[1] == 't' && low[2] == 't'
            && low[3] == 'r' && low[4] == '_') return 1;
    }
    return 0;
}

uint8_t prepass_lblock_kind(const uint8_t *p, uint32_t start, uint32_t end) {
    if (name_eq_ci(p, start, end, "src"))     return 1;
    if (name_eq_ci(p, start, end, "example")) return 2;
    if (name_eq_ci(p, start, end, "export"))  return 3;
    if (name_eq_ci(p, start, end, "verse"))   return 4;
    if (name_eq_ci(p, start, end, "comment")) return 5;
    return 0;
}

PropdrawerLookahead prepass_propdrawer_lookahead(const uint8_t *line,
                                                 uint32_t line_len,
                                                 int line_has_nul) {
    if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

    /* Headline boundary - mirrors classify_line's own col-0 star-run
     * detection, which terminates every open scope including a
     * property drawer's. */
    if (line_len > 0 && line[0] == '*') {
        uint32_t i = 0;
        while (i < line_len && line[i] == '*') i++;
        if (i == line_len || line[i] == ' ') return PROPDRAWER_LOOKAHEAD_STOP;
    }

    uint16_t indent = leading_indent(line, line_len);
    if (indent >= line_len) return PROPDRAWER_LOOKAHEAD_DISQUALIFY; /* blank */

    const uint8_t *trimmed = line + indent;
    uint32_t rem = line_len - indent;

    uint32_t ns, ne;
    if (is_drawer_line(trimmed, rem, &ns, &ne)
        && name_iequals(trimmed, ns, ne, "END"))
        return PROPDRAWER_LOOKAHEAD_STOP;

    if (!is_node_property(trimmed, rem)) return PROPDRAWER_LOOKAHEAD_DISQUALIFY;

    /* Key shape is valid, but a NUL anywhere on the line - necessarily
     * in the value, since a NUL in the key position already fails the
     * charset check above - still disqualifies: tree-sitter's own
     * generated lexer treats codepoint 0 as an internal EOF sentinel,
     * so the JS-side property_value regex token can never advance past
     * it. */
    if (line_has_nul) return PROPDRAWER_LOOKAHEAD_DISQUALIFY;

    return PROPDRAWER_LOOKAHEAD_OK;
}

static int parse_latexenv(const uint8_t *trimmed, uint32_t rem,
                          int is_end,
                          uint32_t *name_start, uint32_t *name_end) {
    const char *kw = is_end ? "\\end{" : "\\begin{";
    size_t klen = strlen(kw);
    if (rem < klen + 2) return 0;
    if (memcmp(trimmed, kw, klen) != 0) return 0;
    uint32_t i = (uint32_t)klen;
    while (i < rem && trimmed[i] != '}') i++;
    if (i == klen || i >= rem) return 0;
    *name_start = (uint32_t)klen;
    *name_end = i;
    return 1;
}

static int parse_list_bullet(const uint8_t *trimmed, uint32_t rem,
                             int trimmed_at_col_zero,
                             uint32_t *kind, uint32_t *bullet_end,
                             uint32_t *checkbox, uint32_t *counter) {
    if (rem < 2) return 0;

    *kind = 0;
    *checkbox = 0;
    *counter = 0;
    *bullet_end = 0;

    if (trimmed[0] == '-' && trimmed[1] == ' ') {
        *kind = 1; *bullet_end = 2;
    } else if (trimmed[0] == '+' && trimmed[1] == ' ') {
        *kind = 2; *bullet_end = 2;
    } else if (trimmed[0] == '*' && trimmed[1] == ' '
               && !trimmed_at_col_zero) {
        *kind = 3; *bullet_end = 2;
    } else if (trimmed[0] >= '0' && trimmed[0] <= '9') {
        uint32_t i = 0;
        while (i < rem && trimmed[i] >= '0' && trimmed[i] <= '9') i++;
        if (i < rem - 1 && (trimmed[i] == '.' || trimmed[i] == ')')
            && trimmed[i + 1] == ' ') {
            *kind = (trimmed[i] == '.') ? 4 : 5;
            *bullet_end = i + 2;
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    {
        uint32_t i = *bullet_end;
        if (i + 3 < rem && trimmed[i] == '[' && trimmed[i + 1] == '@') {
            uint32_t j = i + 2;
            uint32_t n = 0;
            while (j < rem && trimmed[j] >= '0' && trimmed[j] <= '9') {
                n = n * 10 + (trimmed[j] - '0');
                j++;
            }
            if (j < rem && trimmed[j] == ']' && j + 1 < rem
                && trimmed[j + 1] == ' ') {
                *counter = n;
                *bullet_end = j + 2;
            }
        }
    }

    {
        uint32_t i = *bullet_end;
        if (i + 3 < rem && trimmed[i] == '['
            && (trimmed[i + 1] == ' ' || trimmed[i + 1] == 'X'
                || trimmed[i + 1] == 'x' || trimmed[i + 1] == '-')
            && trimmed[i + 2] == ']'
            && (i + 3 >= rem || trimmed[i + 3] == ' ')) {
            uint8_t c = trimmed[i + 1];
            *checkbox = (c == ' ') ? 1 : (c == '-') ? 3 : 2;
            *bullet_end = i + 3 + (i + 3 < rem && trimmed[i + 3] == ' ' ? 1 : 0);
        }
    }

    return 1;
}

static LineTokenType classify_line(struct prepass_state *s,
                                   const uint8_t *line, uint32_t line_len,
                                   uint16_t indent, uint64_t *out_meta) {
    *out_meta = 0;

    if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

    if (indent >= line_len) return TT_EMPTY;

    if (indent == 0 && line_len >= 5 && line[0] == '['
        && line[1] == 'f' && line[2] == 'n' && line[3] == ':') {
        uint32_t i = 4;
        while (i < line_len && line[i] != ']'
               && ((line[i] >= 'A' && line[i] <= 'Z') ||
                   (line[i] >= 'a' && line[i] <= 'z') ||
                   (line[i] >= '0' && line[i] <= '9') ||
                   line[i] == '_' || line[i] == '-')) i++;
        if (i > 4 && i < line_len && line[i] == ']') {
            return TT_FOOTNOTE_DEF;
        }
    }

    if (indent == 0 && line[0] == '*') {
        uint32_t i = 0;
        while (i < line_len && line[i] == '*') i++;
        if (i < line_len && line[i] == ' ') {
            if (i < ORG_INLINETASK_MIN_LEVEL) {
                /* A headline is recognised in EVERY scope and terminates
                 * all of them (Emacs: `org-at-heading-p` is t on `^\*+ `
                 * even inside #+begin_.../#+end_...; a literal star in a
                 * block must be escaped `,*`, which fails the line[0]
                 * check above and stays block content). */
                s->depth = 0;
                *out_meta = (uint64_t)(i & 0xff);
                return TT_HEADING;
            }
            /* 15+-star inlinetask lines are only structural outside
             * verbatim scopes; inside a lesser block or latex
             * environment they remain body content. */
            ScopeKind top = scope_top(s);
            if (top != SCOPE_LBLOCK && top != SCOPE_LATEXENV) {
                uint32_t after_star = i + 1;
                if (line_len - after_star == 3
                    && memcmp(line + after_star, "END", 3) == 0
                    && top == SCOPE_INLINETASK) {
                    scope_pop(s);
                    return TT_INLINETASK_CLOSE;
                }
                if (top != SCOPE_INLINETASK) {
                    scope_push(s, SCOPE_INLINETASK);
                }
                return TT_INLINETASK_OPEN;
            }
        }
    }

    const uint8_t *trimmed = line + indent;
    uint32_t rem = line_len - indent;

    if (scope_top(s) == SCOPE_LBLOCK) {
        if (has_prefix_ci(line + indent, line_len - indent, "#+end_")) {
            scope_pop(s);
            return TT_LBLOCK_CLOSE;
        }
        return TT_LBLOCK_BODY;
    }

    if (scope_top(s) == SCOPE_LATEXENV) {
        uint32_t ns, ne;
        if (parse_latexenv(trimmed, rem, 1, &ns, &ne)) {
            scope_pop(s);
            return TT_LATEXENV_CLOSE;
        }
        return TT_LATEXENV_BODY;
    }

    if (scope_top(s) == SCOPE_PROPDRAWER) {
        uint32_t ns, ne;
        if (is_drawer_line(trimmed, rem, &ns, &ne)
            && name_iequals(trimmed, ns, ne, "END")) {
            scope_pop(s);
            return TT_PROPDRAWER_CLOSE;
        }
        if (is_node_property(trimmed, rem)) return TT_NODE_PROPERTY;
        return TT_BODY;
    }

    if (scope_top(s) == SCOPE_DRAWER) {
        uint32_t ns, ne;
        if (is_drawer_line(trimmed, rem, &ns, &ne)
            && name_iequals(trimmed, ns, ne, "END")) {
            scope_pop(s);
            return TT_DRAWER_CLOSE;
        }
        /* Anything else falls through to the general dispatch below
         * (table, list, block, nested drawer, ...) - a drawer body is
         * just another `_content_line` sequence, same as a greater
         * block's (SCOPE_GBLOCK, handled the same way below). */
    }

    if (has_prefix_ci(trimmed, rem, "#+end_")) {
        if (scope_top(s) == SCOPE_GBLOCK) {
            scope_pop(s);
            return TT_GBLOCK_CLOSE;
        }
        return TT_BODY;
    }
    if (has_prefix_ci(trimmed, rem, "#+end:")) {
        if (scope_top(s) == SCOPE_DYNBLOCK) {
            scope_pop(s);
            return TT_DYNBLOCK_CLOSE;
        }
        return TT_BODY;
    }

    if (has_prefix_ci(trimmed, rem, "#+begin_")) {
        uint32_t ns, ne;
        if (parse_block_name(trimmed, rem, 8, &ns, &ne)) {
            /* A lesser-block name must end at whitespace/EOL, matching
             * the scanner's lblock_kind_at boundary check; a name run
             * cut short by some other byte (`#+begin_src.`) takes the
             * full non-whitespace run as the block name instead, per
             * Emacs, and is a greater block. */
            if (prepass_lblock_kind(trimmed, ns, ne)
                && (ne == rem || trimmed[ne] == ' ' || trimmed[ne] == '\t')) {
                scope_push(s, SCOPE_LBLOCK);
                return TT_LBLOCK_OPEN;
            }
            scope_push(s, SCOPE_GBLOCK);
            return TT_GBLOCK_OPEN;
        }
    }
    if (has_prefix_ci(trimmed, rem, "#+begin:")) {
        scope_push(s, SCOPE_DYNBLOCK);
        return TT_DYNBLOCK_OPEN;
    }

    {
        uint32_t ns, ne;
        if (parse_latexenv(trimmed, rem, 0, &ns, &ne)) {
            scope_push(s, SCOPE_LATEXENV);
            return TT_LATEXENV_OPEN;
        }
    }

    {
        uint32_t ns, ne;
        if (is_drawer_line(trimmed, rem, &ns, &ne)) {
            /* `:PROPERTIES:` / `:END:` are case-insensitive in Emacs
             * via `case-fold-search = t` on org's regexes. */
            if (name_iequals(trimmed, ns, ne, "PROPERTIES")) {
                scope_push(s, SCOPE_PROPDRAWER);
                return TT_PROPDRAWER_OPEN;
            }
            if (name_iequals(trimmed, ns, ne, "END")) {
                return TT_BODY;
            }
            scope_push(s, SCOPE_DRAWER);
            return TT_DRAWER_OPEN;
        }
    }

    if (rem >= 3 && trimmed[0] == '#' && trimmed[1] == '+') {
        uint32_t i = 2;
        while (i < rem && (
            (trimmed[i] >= 'A' && trimmed[i] <= 'Z') ||
            (trimmed[i] >= 'a' && trimmed[i] <= 'z') ||
            (trimmed[i] >= '0' && trimmed[i] <= '9') ||
            trimmed[i] == '_' || trimmed[i] == '-'
        )) i++;
        if (i > 2 && i < rem && trimmed[i] == ':') {
            if (is_affiliated_keyword_name(trimmed, 2, i))
                return TT_AFFILIATED_KEYWORD;
            return TT_KEYWORD;
        }
    }

    if (rem >= 1 && trimmed[0] == '#'
        && (rem == 1 || trimmed[1] == ' ' || trimmed[1] == '\t')) {
        return TT_COMMENT;
    }

    if (rem >= 1 && trimmed[0] == ':'
        && (rem == 1 || trimmed[1] == ' ' || trimmed[1] == '\t')) {
        return TT_FIXED_WIDTH;
    }

    if (rem >= 5 && trimmed[0] == '-') {
        uint32_t i = 0;
        while (i < rem && trimmed[i] == '-') i++;
        if (i >= 5) {
            uint32_t j = i;
            while (j < rem && (trimmed[j] == ' ' || trimmed[j] == '\t')) j++;
            if (j == rem) return TT_HRULE;
        }
    }

    {
        uint32_t kind, bullet_end, checkbox, counter;
        int trimmed_at_col_zero = (indent == 0);
        if (parse_list_bullet(trimmed, rem, trimmed_at_col_zero,
                              &kind, &bullet_end, &checkbox, &counter)) {
            *out_meta = ((uint64_t)kind & 0xff)
                      | (((uint64_t)checkbox & 0xff) << 8)
                      | (((uint64_t)counter & 0xffff) << 16)
                      | (((uint64_t)(bullet_end + indent) & 0xffff) << 32);
            return TT_LIST_ITEM;
        }
    }

    if (rem >= 1 && trimmed[0] == '|') {
        int is_rule = 1;
        for (uint32_t i = 1; i < rem; i++) {
            uint8_t c = trimmed[i];
            if (c != '|' && c != '-' && c != '+' && c != ' ' && c != '\t') {
                is_rule = 0; break;
            }
        }
        return is_rule ? TT_TABLE_RULE : TT_TABLE_ROW;
    }

    if (is_planning(trimmed, rem)) return TT_PLANNING;
    if (has_prefix_ci(trimmed, rem, "CLOCK:")) return TT_CLOCK;
    if (rem >= 4 && trimmed[0] == '%' && trimmed[1] == '%'
        && trimmed[2] == '(' && trimmed[rem - 1] == ')') {
        return TT_DIARY_SEXP;
    }
    /* Active-timestamp diary sexp form: `<%%(...)>` */
    if (rem >= 6 && trimmed[0] == '<' && trimmed[1] == '%' && trimmed[2] == '%'
        && trimmed[3] == '(' && trimmed[rem - 2] == ')' && trimmed[rem - 1] == '>') {
        return TT_DIARY_SEXP;
    }

    return TT_BODY;
}

size_t prepass_serialize(const prepass_state_t *s,
                         uint8_t *buffer, size_t buffer_capacity) {
    const struct prepass_state *st = (const struct prepass_state *)s;
    size_t needed = 1u + (size_t)st->depth;
    if (buffer == NULL || buffer_capacity < needed) return needed;
    buffer[0] = st->depth;
    if (st->depth > 0) memcpy(buffer + 1, st->stack, st->depth);
    return needed;
}

int prepass_deserialize(prepass_state_t *s,
                        const uint8_t *buffer, size_t buffer_size) {
    struct prepass_state *st = (struct prepass_state *)s;
    if (buffer_size < 1) return 0;
    uint8_t depth = buffer[0];
    if (depth > PREPASS_STACK_MAX) return 0;
    if (buffer_size < 1u + (size_t)depth) return 0;
    st->depth = depth;
    if (depth > 0) memcpy(st->stack, buffer + 1, depth);
    return 1;
}
