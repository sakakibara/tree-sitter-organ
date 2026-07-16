/* C-level tests for the external scanner and prepass.  Includes
 * scanner.c directly for access to ScannerState and the externals
 * enum; run under ASan/UBSan via `make check-c`. */
#include "../src/scanner.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define N_EXTERNALS (EXT_ITEM_TAG_SEP + 1)

typedef struct {
    TSLexer lexer;
    const char *src;
    uint32_t len;
    uint32_t pos;
    uint32_t mark;
} MockLexer;

static void mock_sync(MockLexer *m) {
    m->lexer.lookahead = (m->pos < m->len) ? (int32_t)(uint8_t)m->src[m->pos] : 0;
}

static void mock_advance(TSLexer *l, bool skip) {
    (void)skip;
    MockLexer *m = (MockLexer *)l;
    if (m->pos < m->len) m->pos++;
    mock_sync(m);
}

static void mock_mark_end(TSLexer *l) {
    MockLexer *m = (MockLexer *)l;
    m->mark = m->pos;
}

static uint32_t mock_get_column(TSLexer *l) {
    MockLexer *m = (MockLexer *)l;
    uint32_t col = 0;
    for (uint32_t i = m->pos; i > 0 && m->src[i - 1] != '\n'; i--) col++;
    return col;
}

static bool mock_included_range_start(const TSLexer *l) {
    (void)l;
    return false;
}

static bool mock_eof(const TSLexer *l) {
    const MockLexer *m = (const MockLexer *)l;
    return m->pos >= m->len;
}

static void mock_log(const TSLexer *l, const char *fmt, ...) {
    (void)l; (void)fmt;
}

static void mock_init(MockLexer *m, const char *src) {
    memset(m, 0, sizeof(*m));
    m->lexer.advance = mock_advance;
    m->lexer.mark_end = mock_mark_end;
    m->lexer.get_column = mock_get_column;
    m->lexer.is_at_included_range_start = mock_included_range_start;
    m->lexer.eof = mock_eof;
    m->lexer.log = mock_log;
    m->src = src;
    m->len = (uint32_t)strlen(src);
    mock_sync(m);
}

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* Scan one token with exactly the given symbols valid.  Never pass
 * every symbol: the body-text branch (comment/fixed-width) fires
 * first when its symbols are valid and would mask the code path
 * under test. */
static bool scan_with(void *scanner, MockLexer *m,
                      const int *syms, size_t n_syms) {
    bool valid[N_EXTERNALS];
    memset(valid, false, sizeof(valid));
    for (size_t i = 0; i < n_syms; i++) valid[syms[i]] = true;
    return tree_sitter_org_external_scanner_scan(scanner, &m->lexer, valid);
}

static void test_deep_indent_bullet_no_overflow(void) {
    /* 63 spaces of indent + "- x": the bullet writes previously ran
     * past bullet_consumed[64].  ASan fails this test pre-fix. */
    static const int list_syms[] = {
        EXT_LIST_ITEM_BULLET, EXT_PLAIN_LIST_OPEN, EXT_PLAIN_LIST_CLOSE,
    };
    static const uint32_t indents[] = { 62, 63, 64, 65, 80, 200 };
    for (size_t i = 0; i < sizeof(indents) / sizeof(indents[0]); i++) {
        char buf[512];
        uint32_t n = indents[i];
        memset(buf, ' ', n);
        snprintf(buf + n, sizeof(buf) - n, "- x\n");
        void *s = tree_sitter_org_external_scanner_create();
        MockLexer m;
        mock_init(&m, buf);
        scan_with(s, &m, list_syms, 3);
        tree_sitter_org_external_scanner_destroy(s);

        /* Numeric bullet variant exercises the digit-run writes. */
        memset(buf, ' ', n);
        snprintf(buf + n, sizeof(buf) - n, "12. x\n");
        s = tree_sitter_org_external_scanner_create();
        mock_init(&m, buf);
        scan_with(s, &m, list_syms, 3);
        tree_sitter_org_external_scanner_destroy(s);
    }
}

static void test_deserialize_zero_resets_state(void) {
    ScannerState *s =
        (ScannerState *)tree_sitter_org_external_scanner_create();
    /* Dirty every field a prior parse could leave behind. */
    s->heading_depth = 3;
    s->heading_levels[0] = 1; s->heading_levels[1] = 2; s->heading_levels[2] = 3;
    s->pending_closes = 2;
    s->pending_open_level = 4;
    s->list_depth = 2;
    s->list_indents[0] = 0; s->list_indents[1] = 2;
    s->pending_list_closes = 1;
    s->pending_list_open_indent = 6;
    s->lblock_kind = 1;
    s->at_item_def = 1;
    {   /* push a prepass scope */
        const char *line = "#+begin_src lua";
        prepass_classify_line(s->prepass, (const uint8_t *)line,
                              (uint32_t)strlen(line));
        CHECK(prepass_scope_top(s->prepass) == SCOPE_LBLOCK);
    }

    tree_sitter_org_external_scanner_deserialize(s, NULL, 0);

    CHECK(s->heading_depth == 0);
    CHECK(s->pending_closes == 0);
    CHECK(s->pending_open_level == 0);
    CHECK(s->list_depth == 0);
    CHECK(s->pending_list_closes == 0);
    CHECK(s->pending_list_open_indent == -1);
    CHECK(s->lblock_kind == 0);
    CHECK(s->at_item_def == 0);
    CHECK(prepass_scope_top(s->prepass) == SCOPE_NONE);

    /* EOF scan on empty input must emit nothing (no phantom closes).
     * Pre-fix, the surviving heading_depth makes this emit a
     * zero-width _heading_close. */
    MockLexer m;
    mock_init(&m, "");
    static const int close_syms[] = {
        EXT_HEADING_CLOSE, EXT_PLAIN_LIST_CLOSE,
    };
    CHECK(scan_with(s, &m, close_syms, 2) == false);

    tree_sitter_org_external_scanner_destroy(s);
}

static void test_deserialize_corrupt_buffer_resets_state(void) {
    ScannerState *s =
        (ScannerState *)tree_sitter_org_external_scanner_create();
    s->heading_depth = 2;
    s->heading_levels[0] = 1; s->heading_levels[1] = 2;
    char blob[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned n = tree_sitter_org_external_scanner_serialize(s, blob);
    CHECK(n > 0);

    /* Roundtrip sanity. */
    tree_sitter_org_external_scanner_deserialize(s, blob, n);
    CHECK(s->heading_depth == 2 && s->heading_levels[1] == 2);

    /* Corrupt heading depth: state must come out CLEAN, not partial. */
    blob[0] = (char)200;
    tree_sitter_org_external_scanner_deserialize(s, blob, n);
    CHECK(s->heading_depth == 0);
    CHECK(s->pending_list_open_indent == -1);
    CHECK(prepass_scope_top(s->prepass) == SCOPE_NONE);

    /* Truncated buffer: same guarantee. */
    blob[0] = 2;
    tree_sitter_org_external_scanner_deserialize(s, blob, 3);
    CHECK(s->heading_depth == 0);
    CHECK(s->pending_list_open_indent == -1);

    tree_sitter_org_external_scanner_destroy(s);
}

int main(void) {
    test_deserialize_zero_resets_state();
    test_deserialize_corrupt_buffer_resets_state();
    test_deep_indent_bullet_no_overflow();
    if (failures > 0) {
        fprintf(stderr, "scanner_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("scanner_tests: all tests passed\n");
    return 0;
}
