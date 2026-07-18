/* C-level tests for the external scanner and prepass.  Includes
 * scanner.c directly for access to ScannerState and the externals
 * enum; run under ASan/UBSan via `make check-c`. */
#include "../src/scanner.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern uint16_t organ_leading_indent_scalar(const uint8_t *p, uint32_t len);
extern uint16_t organ_leading_indent_swar(const uint8_t *p, uint32_t len);

#define N_EXTERNALS (EXT_FN_EMPTY_LINE + 1)

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

    s->blank_run = 1;
    n = tree_sitter_org_external_scanner_serialize(s, blob);
    s->blank_run = 0;
    tree_sitter_org_external_scanner_deserialize(s, blob, n);
    CHECK(s->blank_run == 1);

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

static void check_indent_case(const uint8_t *p, uint32_t len) {
    uint16_t a = organ_leading_indent_scalar(p, len);
    uint16_t b = organ_leading_indent_swar(p, len);
    if (a != b) {
        fprintf(stderr, "FAIL indent mismatch scalar=%u swar=%u len=%u: ",
                a, b, len);
        for (uint32_t i = 0; i < len && i < 16; i++)
            fprintf(stderr, "%02x ", p[i]);
        fprintf(stderr, "\n");
        failures++;
    }
}

static void test_swar_indent_matches_scalar(void) {
    check_indent_case((const uint8_t *)" ! - foo", 8);
    check_indent_case((const uint8_t *)" !! hello", 9);
    check_indent_case((const uint8_t *)"\t\x08 x pad!", 9);
    check_indent_case((const uint8_t *)"        ", 8);
    check_indent_case((const uint8_t *)"\t\t\t\t\t\t\t\t", 8);

    /* Full byte sweep at every lane offset: prefix of 0..7 spaces,
     * then byte b, then a tail long enough to fill the SWAR word. */
    uint8_t buf[24];
    for (uint32_t off = 0; off < 8; off++) {
        for (int b = 0; b < 256; b++) {
            memset(buf, ' ', off);
            buf[off] = (uint8_t)b;
            memset(buf + off + 1, 'x', sizeof(buf) - off - 1);
            check_indent_case(buf, (uint32_t)sizeof(buf));
        }
    }
}

static void test_classify_rollback_on_failed_scan(void) {
    ScannerState *s =
        (ScannerState *)tree_sitter_org_external_scanner_create();
    /* A line that pushes SCOPE_GBLOCK during classification, scanned
     * while its token is NOT valid: the scan fails and the scope
     * stack must be unchanged. */
    MockLexer m;
    mock_init(&m, "#+begin_quote\nbody\n");
    bool valid[N_EXTERNALS];
    memset(valid, false, sizeof(valid));
    valid[EXT_EMPTY_LINE] = true;   /* anything except gblock open */
    bool ok = tree_sitter_org_external_scanner_scan(s, &m.lexer, valid);
    CHECK(ok == false);
    CHECK(prepass_scope_top(s->prepass) == SCOPE_NONE);
    CHECK(s->lblock_kind == 0);

    /* Same for a lesser block: lblock_kind must not stick either. */
    mock_init(&m, "#+begin_src lua\nx\n");
    ok = tree_sitter_org_external_scanner_scan(s, &m.lexer, valid);
    CHECK(ok == false);
    CHECK(prepass_scope_top(s->prepass) == SCOPE_NONE);
    CHECK(s->lblock_kind == 0);

    tree_sitter_org_external_scanner_destroy(s);
}

static void test_planning_token_is_not_zero_width(void) {
    ScannerState *s =
        (ScannerState *)tree_sitter_org_external_scanner_create();
    MockLexer m;
    mock_init(&m, "SCHEDULED: <2026-05-01 Fri>\n");
    bool valid[N_EXTERNALS];
    memset(valid, false, sizeof(valid));
    valid[EXT_PLANNING_LINE] = true;
    bool ok = tree_sitter_org_external_scanner_scan(s, &m.lexer, valid);
    CHECK(ok == true);
    CHECK(m.lexer.result_symbol == EXT_PLANNING_LINE);
    CHECK(m.mark == 10);   /* token covers "SCHEDULED:" */
    tree_sitter_org_external_scanner_destroy(s);
}

int main(void) {
    test_deserialize_zero_resets_state();
    test_deserialize_corrupt_buffer_resets_state();
    test_deep_indent_bullet_no_overflow();
    test_swar_indent_matches_scalar();
    test_classify_rollback_on_failed_scan();
    test_planning_token_is_not_zero_width();
    if (failures > 0) {
        fprintf(stderr, "scanner_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("scanner_tests: all tests passed\n");
    return 0;
}
