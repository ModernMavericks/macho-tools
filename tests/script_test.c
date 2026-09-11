/*
 * tests/script_test.c — hermetic tests for src/script.c.
 *
 * Ground truth is hand-written script text and the field vector it must
 * produce, so this is host-agnostic: no fixture file, no toolchain
 * dependence. The quoting cases are the ones mt_quote (compat/translate.sh)
 * actually emits, because the spec requires the generator and this parser
 * cannot drift.
 *
 * Build: clang -O2 -Wall -Isrc -o /tmp/scripttest tests/script_test.c \
 *   src/script.c && /tmp/scripttest
 */
#include "script.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

static void split_is(const char *in, int want_n, const char *w0,
                     const char *w1, const char *w2) {
    char buf[512]; char *av[8]; char err[128] = {0};
    snprintf(buf, sizeof buf, "%s", in);
    int n = ms_split(buf, av, 8, err, sizeof err);
    CHECK(n == want_n, "[%s] -> %d fields, wanted %d (err: %s)", in, n, want_n, err);
    if (n != want_n) return;
    if (w0) CHECK(strcmp(av[0], w0) == 0, "[%s] field0 = '%s', wanted '%s'", in, av[0], w0);
    if (w1) CHECK(strcmp(av[1], w1) == 0, "[%s] field1 = '%s', wanted '%s'", in, av[1], w1);
    if (w2) CHECK(strcmp(av[2], w2) == 0, "[%s] field2 = '%s', wanted '%s'", in, av[2], w2);
}

static void test_plain_fields(void) {
    split_is("dylib replace A B", 4, "dylib", "replace", "A");
    split_is("  load-command   delete   uuid  ", 3, "load-command", "delete", "uuid");
}

static void test_blank_and_comment(void) {
    split_is("", 0, NULL, NULL, NULL);
    split_is("   ", 0, NULL, NULL, NULL);
    split_is("# a whole-line comment", 0, NULL, NULL, NULL);
    split_is("   # indented comment", 0, NULL, NULL, NULL);
    split_is("dylib append /x # trailing comment", 3, "dylib", "append", "/x");
}

/* mt_quote wraps anything outside [A-Za-z0-9_@%+=:,./-] in single quotes,
 * so every one of these is a shape the generator really emits. */
static void test_mt_quote_shapes(void) {
    split_is("dylib append '/a path/with spaces.dylib'", 3,
             "dylib", "append", "/a path/with spaces.dylib");
    split_is("dylib append '/has#hash'", 3, "dylib", "append", "/has#hash");
    split_is("dylib append '/has$(cmd)'", 3, "dylib", "append", "/has$(cmd)");
    split_is("dylib append '/has;semi'", 3, "dylib", "append", "/has;semi");
    split_is("dylib append ''", 3, "dylib", "append", "");
    /* mt_quote's '\'' idiom for an embedded single quote */
    split_is("dylib append '/it'\\''s'", 3, "dylib", "append", "/it's");
}

static void test_double_quotes_and_backslash(void) {
    split_is("dylib append \"/a path\"", 3, "dylib", "append", "/a path");
    split_is("dylib append \"/esc\\\"q\"", 3, "dylib", "append", "/esc\"q");
    split_is("dylib append /lead\\ space", 3, "dylib", "append", "/lead space");
}

static void test_unterminated_quote_is_an_error(void) {
    char buf[64]; char *av[8]; char err[128] = {0};
    snprintf(buf, sizeof buf, "dylib append '/unterminated");
    int n = ms_split(buf, av, 8, err, sizeof err);
    CHECK(n == -1, "an unterminated quote is an error (got %d)", n);
    CHECK(err[0] != 0, "and says so");
}

static void test_too_many_fields_is_an_error(void) {
    char buf[64]; char *av[2]; char err[128] = {0};
    snprintf(buf, sizeof buf, "a b c d");
    int n = ms_split(buf, av, 2, err, sizeof err);
    CHECK(n == -1, "overflowing the field vector is an error, not truncation (got %d)", n);
}

static void test_parses_the_production_script(void) {
    static const char src[] =
        "# Claude Code -> 10.9\n"
        "fixups        set      classic\n"
        "version-min   set      10.9\n"
        "load-command  delete   uuid\n"
        "dylib         replace  /usr/lib/libSystem.B.dylib  @loader_path/../S.dylib\n";
    ms_script s; char err[256] = {0};
    int r = ms_parse(src, sizeof src - 1, &s, err, sizeof err);
    CHECK(r == 0, "the production script parses (got %d, err: %s)", r, err);
    if (r != 0) return;
    CHECK(s.n == 4, "four statements (got %d)", s.n);
    CHECK(s.stmts[0].kind == MS_FIXUPS && s.stmts[0].op == MS_SET, "stmt0 is fixups set");
    CHECK(s.stmts[3].kind == MS_DYLIB && s.stmts[3].op == MS_REPLACE, "stmt3 is dylib replace");
    CHECK(strcmp(s.stmts[3].b, "@loader_path/../S.dylib") == 0, "stmt3 operand b");
    CHECK(s.stmts[3].line == 5, "stmt3 remembers its source line (got %d)", s.stmts[3].line);
    ms_free(&s);
}

static void test_directives_set_flags_and_are_not_statements(void) {
    static const char src[] = "allow-grow\nfatal-warnings\nload-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "parses (%s)", err);
    CHECK(s.allow_grow == 1, "allow-grow set the flag");
    CHECK(s.fatal_warnings == 1, "fatal-warnings set the flag");
    CHECK(s.n == 1, "directives are not statements (got n=%d)", s.n);
    ms_free(&s);
}

static void test_a_directive_after_an_operation_is_an_error(void) {
    static const char src[] = "load-command delete uuid\nallow-grow\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "a directive after an operation is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names the line (got: %s)", err);
}

static void test_unknown_statement_and_wrong_arity(void) {
    ms_script s; char err[256] = {0};
    static const char bad1[] = "frobnicate all\n";
    CHECK(ms_parse(bad1, sizeof bad1 - 1, &s, err, sizeof err) == -1, "unknown kind refused");
    static const char bad2[] = "segment rename __ONLYONE\n";
    CHECK(ms_parse(bad2, sizeof bad2 - 1, &s, err, sizeof err) == -1, "wrong arity refused");
    static const char bad3[] = "load-command delete not-a-kind\n";
    CHECK(ms_parse(bad3, sizeof bad3 - 1, &s, err, sizeof err) == -1, "unknown KIND refused");
}

static void test_no_operation_cap(void) {
    /* The old CLI capped at MR_MAX_OPS (32). The spec is explicit that edit
     * sizes from the parsed script, because the dominant real workload --
     * repointing every framework in frameworks.json at a stub -- is 32
     * dylib replaces plus everything else. */
    char big[64 * 1024]; size_t len = 0;
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(big + len, sizeof big - len,
                                "dylib replace /a/%d.dylib /b/%d.dylib\n", i, i);
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(big, len, &s, err, sizeof err) == 0, "200 statements parse (%s)", err);
    CHECK(s.n == 200, "all 200 kept (got %d)", s.n);
    ms_free(&s);
}

int main(void) {
    test_plain_fields();
    test_blank_and_comment();
    test_mt_quote_shapes();
    test_double_quotes_and_backslash();
    test_unterminated_quote_is_an_error();
    test_too_many_fields_is_an_error();
    test_parses_the_production_script();
    test_directives_set_flags_and_are_not_statements();
    test_a_directive_after_an_operation_is_an_error();
    test_unknown_statement_and_wrong_arity();
    test_no_operation_cap();
    printf("script_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
