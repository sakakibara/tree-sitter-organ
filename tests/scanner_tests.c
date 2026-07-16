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

int main(void) {
    test_deep_indent_bullet_no_overflow();
    if (failures > 0) {
        fprintf(stderr, "scanner_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("scanner_tests: all tests passed\n");
    return 0;
}
