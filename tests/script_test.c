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
#include "arch_names.h"
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

/* A backslash escapes the byte after it, so a line ending in one has
 * nothing to escape. ms_split must say so rather than copy the terminating
 * NUL as the escaped byte and keep reading past the end of the line. */
static void test_trailing_backslash_is_an_error(void) {
    /* Zeroed past the NUL, so a regression reads zeros rather than stack
     * garbage, and fails the same way every run. */
    char buf[64] = {0}; char *av[8]; char err[128] = {0};
    snprintf(buf, sizeof buf, "dylib append /x\\");
    int n = ms_split(buf, av, 8, err, sizeof err);
    CHECK(n == -1, "a trailing backslash is an error (got %d)", n);
    CHECK(strcmp(err, "trailing backslash") == 0,
          "and says so (got: %s)", err);
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

/* A use-after-free regression test. Every ms_parse failure path that quotes
 * a field in its message must format that message BEFORE freeing the
 * storage the field points into; ms_parse once freed first.
 *
 * On its own this assertion does not reliably catch a regression: libc's
 * allocator typically leaves a freed block's bytes untouched until that
 * memory is reused, so a use-after-free read here often reads back the
 * original text anyway, by pure luck. What makes it fail is
 * MallocScribble=1, which overwrites every freed block with 0x55 on free --
 * CMakeLists.txt sets that in script_test's ctest ENVIRONMENT property so
 * this test (and any future one like it) is exercised for real, not just
 * when someone happens to run the binary by hand with the right variable
 * set. */
static void test_error_message_quoting_survives_the_free(void) {
    static const char src[] = "frobnicate all\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1, "unknown statement refused");
    CHECK(strstr(err, "frobnicate") != NULL,
          "the message still quotes the field, not freed/scribbled memory (got: %s)", err);
}

/* An embedded NUL used to make ms_split stop early and silently hand back a
 * truncated field -- e.g. "dylib replace /a /b\0.dylib" parsed with
 * b="/b". Now any control byte except tab (a field separator) and newline
 * (the line separator) is refused, CR included, which also closes the CRLF
 * case. */
static void test_embedded_nul_is_refused(void) {
    static const char src[] = "dylib replace /a /b\0.dylib\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "an embedded NUL is refused, not silently truncated");
    CHECK(strstr(err, "line 1") != NULL, "and names the line (got: %s)", err);
    CHECK(strstr(err, "control character") != NULL, "and names what it is (got: %s)", err);
}

static void test_crlf_is_refused(void) {
    static const char src[] = "dylib replace /a /b\r\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "a CR is refused, not folded into the operand");
    CHECK(strstr(err, "line 1") != NULL, "and names the line (got: %s)", err);
    CHECK(strstr(err, "control character") != NULL, "and names what it is (got: %s)", err);
}

/* The spec's directive rules: repeating one is idempotent, and a directive
 * takes no operands. */

static void test_repeated_directive_is_accepted(void) {
    static const char src[] = "allow-grow\nallow-grow\nfatal-warnings\nfatal-warnings\ndylib delete /x\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0,
          "repeating a directive is idempotent, not an error (%s)", err);
    CHECK(s.allow_grow == 1, "allow-grow still set");
    CHECK(s.fatal_warnings == 1, "fatal-warnings still set");
    CHECK(s.n == 1, "only the operation counts as a statement (got %d)", s.n);
    ms_free(&s);
}

static void test_directive_with_operand_is_refused(void) {
    static const char src[] = "allow-grow yes\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "a directive given an operand is refused");
    CHECK(strstr(err, "line 1") != NULL, "and names the line (got: %s)", err);
    CHECK(strstr(err, "takes no operands") != NULL, "and names why (got: %s)", err);
}

/* The `if (s.n != 2) { ms_free(&s); return; }` guard this used to have,
 * placed right before `CHECK(s.n == 2, ...)`, made that CHECK unreachable
 * in its failing case -- a mutation dropping the final, newline-less line
 * still reported 0 failures. The s.n==2 CHECK now always runs; only the
 * stmts[1] INDEXING is guarded, and by a positive condition that doesn't
 * skip the count check it's guarding against. */
static void test_final_line_without_newline_parses(void) {
    static const char src[] = "load-command delete uuid\ndylib replace /a /b";
    ms_script s; char err[256] = {0};
    int r = ms_parse(src, sizeof src - 1, &s, err, sizeof err);
    CHECK(r == 0, "a final line without a trailing newline parses (%s)", err);
    if (r != 0) return;
    CHECK(s.n == 2, "both statements kept (got %d)", s.n);
    if (s.n == 2)
        CHECK(s.stmts[1].line == 2, "second stmt remembers line 2 (got %d)", s.stmts[1].line);
    ms_free(&s);
}

/* Guarded the same way as the test above, for the same reason: the count
 * CHECK must run even when the count is wrong. */
static void test_blank_and_comment_lines_dont_shift_line_numbers(void) {
    static const char src[] =
        "load-command delete uuid\n"
        "\n"
        "# a comment\n"
        "   \n"
        "dylib delete /x\n";
    ms_script s; char err[256] = {0};
    int r = ms_parse(src, sizeof src - 1, &s, err, sizeof err);
    CHECK(r == 0, "parses (%s)", err);
    if (r != 0) return;
    CHECK(s.n == 2, "two statements; blank/comment lines aren't counted (got %d)", s.n);
    if (s.n == 2) {
        CHECK(s.stmts[0].line == 1, "stmt0 is line 1 (got %d)", s.stmts[0].line);
        CHECK(s.stmts[1].line == 5, "stmt1 is line 5, not shifted by the skipped lines (got %d)", s.stmts[1].line);
    }
    ms_free(&s);
}

static void test_version_min_value_refusal(void) {
    static const char src[] = "version-min set 10.10\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "version-min set 10.10 is refused");
    CHECK(strstr(err, "10.9") != NULL, "names what is accepted (got: %s)", err);
}

static void test_swift_abi_value_refusal(void) {
    static const char src[] = "swift-abi set native\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "swift-abi set native is refused");
    CHECK(strstr(err, "legacy") != NULL, "names what is accepted (got: %s)", err);
}

static void test_fixups_value_refusal(void) {
    static const char src[] = "fixups set chained\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "fixups set chained is refused");
    CHECK(strstr(err, "classic") != NULL, "names what is accepted (got: %s)", err);
}

static void test_kind_and_op_names(void) {
    CHECK(strcmp(ms_kind_name(MS_DYLIB), "dylib") == 0, "ms_kind_name(MS_DYLIB)");
    CHECK(strcmp(ms_kind_name(MS_LOAD_COMMAND), "load-command") == 0, "ms_kind_name(MS_LOAD_COMMAND)");
    CHECK(strcmp(ms_kind_name(MS_RPATH), "rpath") == 0, "ms_kind_name(MS_RPATH)");
    CHECK(strcmp(ms_op_name(MS_REPLACE), "replace") == 0, "ms_op_name(MS_REPLACE)");
    CHECK(strcmp(ms_op_name(MS_DELETE), "delete") == 0, "ms_op_name(MS_DELETE)");
    CHECK(strcmp(ms_op_name(MS_REEXPORT), "reexport") == 0, "ms_op_name(MS_REEXPORT)");
    CHECK(strcmp(ms_kind_name(-1), "unknown") == 0, "ms_kind_name of an out-of-table value");
    CHECK(strcmp(ms_op_name(-1), "unknown") == 0, "ms_op_name of an out-of-table value");
}

/* MS_MAX_FIELDS' comment claims this reports arity, not overflow, for a
 * line with a handful of stray extra fields. Prove it. */
static void test_extra_fields_report_arity_not_overflow(void) {
    static const char src[] = "dylib replace a b c d e f\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1, "extra fields refused");
    CHECK(strstr(err, "argument") != NULL, "names arity (got: %s)", err);
    CHECK(strstr(err, "too many fields") == NULL,
          "does not fall back to ms_split's generic overflow message (got: %s)", err);
}

/* The FIRST error in source order must be reported, not whichever kind of
 * mistake (syntax vs. semantic) some earlier pass happened to notice first.
 * Line 2 here is a semantic error (unknown statement); line 4 is a syntax
 * error (unterminated quote). */
static void test_first_error_reported_is_earliest_in_line_order(void) {
    static const char src[] =
        "dylib delete /x\n"
        "frobnicate x\n"
        "\n"
        "dylib delete 'unterminated\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1, "refused");
    CHECK(strstr(err, "line 2") != NULL,
          "names the earlier (semantic) error's line, not the later (syntax) one (got: %s)", err);
}

/* Walks ms_table_row directly and confirms MS_TABLE has the spec's 15 rows
 * (the "Statements" table's 14 kind/op pairs, plus `target 10.9`, whose
 * profile occupies the op column), each of which round-trips through an
 * actual ms_parse -- not just that one known row's text appears somewhere.
 * tests/cli_test.sh separately counts --capabilities' own "statement " lines
 * (exactly 15, all unique); together the two catch the generator
 * (cli/machotool.c's loop over ms_table_row) and the table itself going out
 * of step with each other -- a dropped, extra, or duplicated line on either
 * side. */
static void test_capabilities_table_round_trips(void) {
    int i, n_rows = 0;
    const char *kind, *op;
    int nargs;
    for (i = 0; ms_table_row(i, &kind, &op, &nargs); i++) {
        char line[256];
        const char *a = "x";
        const char *b = "y";
        /* Every value/KIND gate ms_parse enforces is a separate check from
         * arity, so a dummy operand has to satisfy it too, or this row
         * would be refused for a reason that has nothing to do with what
         * this test is proving. */
        if (strcmp(kind, "load-command") == 0 && strcmp(op, "delete") == 0) a = "uuid";
        else if (strcmp(kind, "version-min") == 0) a = "10.9";
        else if (strcmp(kind, "swift-abi") == 0) a = "legacy";
        else if (strcmp(kind, "fixups") == 0) a = "classic";

        if (nargs == 2)
            snprintf(line, sizeof line, "%s %s %s %s\n", kind, op, a, b);
        else if (nargs == 1)
            snprintf(line, sizeof line, "%s %s %s\n", kind, op, a);
        else
            snprintf(line, sizeof line, "%s %s\n", kind, op);

        {
            ms_script s; char err[256] = {0};
            int r = ms_parse(line, strlen(line), &s, err, sizeof err);
            CHECK(r == 0, "table row '%s %s' parses (%s)", kind, op, err);
            if (r == 0) {
                CHECK(s.n == 1, "table row '%s %s' yields one statement (got %d)", kind, op, s.n);
                if (s.n == 1) {
                    CHECK(strcmp(ms_kind_name(s.stmts[0].kind), kind) == 0,
                          "table row '%s %s': kind round-trips", kind, op);
                    CHECK(strcmp(ms_op_name(s.stmts[0].op), op) == 0,
                          "table row '%s %s': op round-trips", kind, op);
                }
                ms_free(&s);
            }
        }
        n_rows++;
    }
    CHECK(n_rows == 15, "the statement table has 15 rows (got %d)", n_rows);
}

static void test_arch_directive_names_rows(void) {
    static const char src[] = "arch x86_64\narch arm64\narch x86_64\nload-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "arch: parses (%s)", err);
    CHECK(s.arch_mask == ((1u << ma_lookup("x86_64")) | (1u << ma_lookup("arm64"))),
          "arch: the mask names x86_64 and arm64, a repeat harmlessly (got 0x%x)", s.arch_mask);
    CHECK(s.n == 1, "arch: directives are not statements (got %d)", s.n);
    ms_free(&s);
}

static void test_no_arch_directive_is_an_empty_mask(void) {
    static const char src[] = "load-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "no arch: parses (%s)", err);
    CHECK(s.arch_mask == 0, "no arch: the mask is empty (got 0x%x)", s.arch_mask);
    ms_free(&s);
}

static void test_arch_directive_errors(void) {
    ms_script s; char err[256];
    static const char bad1[] = "arch amd64\n";
    err[0] = 0;
    CHECK(ms_parse(bad1, sizeof bad1 - 1, &s, err, sizeof err) == -1, "arch amd64 is refused");
    CHECK(strstr(err, "line 1") && strstr(err, "amd64") && strstr(err, "x86_64, x86_64h"),
          "and names the line, the name, and what is accepted (got: %s)", err);
    static const char bad2[] = "arch\n";
    CHECK(ms_parse(bad2, sizeof bad2 - 1, &s, err, sizeof err) == -1, "arch with no name is refused");
    static const char bad3[] = "arch x86_64 arm64\n";
    CHECK(ms_parse(bad3, sizeof bad3 - 1, &s, err, sizeof err) == -1, "arch with two names is refused");
    static const char bad4[] = "load-command delete uuid\narch x86_64\n";
    CHECK(ms_parse(bad4, sizeof bad4 - 1, &s, err, sizeof err) == -1,
          "arch after an operation is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names line 2 (got: %s)", err);
}

static void test_target_parses_and_is_positional(void) {
    static const char src[] = "allow-grow\ntarget 10.9\nload-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "parses (%s)", err);
    /* target IS a statement -- it occupies a position, because the expansion
     * lands where it is written and position changes what later statements
     * can do (fixups set classic rewrites __LINKEDIT, moving the header pad
     * available to every dylib replace after it). */
    CHECK(s.n == 2, "target occupies a statement slot (got n=%d)", s.n);
    CHECK(s.stmts[0].kind == MS_TARGET, "and it is the first of the two");
    CHECK(s.stmts[0].a == NULL && s.stmts[0].b == NULL,
          "the profile is the statement's op, not an operand");
    CHECK(strcmp(ms_kind_name(s.stmts[0].kind), "target") == 0 &&
          strcmp(ms_op_name(s.stmts[0].op), "10.9") == 0,
          "and both halves name themselves back (got '%s %s')",
          ms_kind_name(s.stmts[0].kind), ms_op_name(s.stmts[0].op));
    ms_free(&s);
}

static void test_two_targets_is_a_parse_error(void) {
    static const char src[] = "target 10.9\ntarget 10.9\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1, "a second target is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names the line (got: %s)", err);
}

static void test_unknown_target_is_refused_not_guessed(void) {
    static const char src[] = "target 10.10\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "an unknown target errors rather than silently doing 10.9's work");
    CHECK(strstr(err, "10.10") != NULL && strstr(err, "10.9") != NULL,
          "and names both what was asked for and what this build knows (got: %s)", err);
    static const char bare[] = "target\n";
    err[0] = 0;
    CHECK(ms_parse(bare, sizeof bare - 1, &s, err, sizeof err) == -1,
          "and a target naming no profile at all is refused too");
    CHECK(strstr(err, "target") != NULL,
          "as a target, not as an unknown statement (got: %s)", err);
}

/* A directive describes the whole run, so it must precede every operation --
 * and `target` is an operation for that purpose, since allow-grow governs the
 * statements its expansion becomes. */
static void test_a_directive_after_target_is_an_error(void) {
    static const char src[] = "target 10.9\nallow-grow\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "allow-grow after target is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names line 2 (got: %s)", err);
}

int main(void) {
    test_plain_fields();
    test_blank_and_comment();
    test_mt_quote_shapes();
    test_double_quotes_and_backslash();
    test_unterminated_quote_is_an_error();
    test_trailing_backslash_is_an_error();
    test_too_many_fields_is_an_error();
    test_parses_the_production_script();
    test_directives_set_flags_and_are_not_statements();
    test_a_directive_after_an_operation_is_an_error();
    test_unknown_statement_and_wrong_arity();
    test_no_operation_cap();
    test_error_message_quoting_survives_the_free();
    test_embedded_nul_is_refused();
    test_crlf_is_refused();
    test_repeated_directive_is_accepted();
    test_directive_with_operand_is_refused();
    test_final_line_without_newline_parses();
    test_blank_and_comment_lines_dont_shift_line_numbers();
    test_version_min_value_refusal();
    test_swift_abi_value_refusal();
    test_fixups_value_refusal();
    test_kind_and_op_names();
    test_extra_fields_report_arity_not_overflow();
    test_first_error_reported_is_earliest_in_line_order();
    test_capabilities_table_round_trips();
    test_arch_directive_names_rows();
    test_no_arch_directive_is_an_empty_mask();
    test_arch_directive_errors();
    test_target_parses_and_is_positional();
    test_two_targets_is_a_parse_error();
    test_unknown_target_is_refused_not_guessed();
    test_a_directive_after_target_is_an_error();
    printf("script_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
