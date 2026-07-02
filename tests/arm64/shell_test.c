/* tests/arm64/shell_test.c - host unit tests for the serial shell core
 * (Issue #80).
 *
 * The tokeniser, dispatcher, and line editor are pure C, so everything a
 * user can do at the console is checked on the host: argv splitting (empty,
 * multiple, tab/space runs, argv overflow), the built-in help listing, known
 * and unknown command dispatch, empty lines, character echo and backspace
 * editing, CR / LF / CRLF line endings, over-long lines - and a fuzz sweep of
 * arbitrary bytes that must never overflow the buffer or crash.
 *
 * Build/run via:  make test-arm-shell
 */

#include "shell.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* ---- capture sink ---- */
static char g_out[8192];
static int  g_out_len;

static void cap_out(void *io, const char *s)
{
    (void)io;
    while (*s && g_out_len < (int)sizeof(g_out) - 1)
        g_out[g_out_len++] = *s++;
    g_out[g_out_len] = '\0';
}

static void cap_reset(void) { g_out_len = 0; g_out[0] = '\0'; }
static int  saw(const char *needle) { return strstr(g_out, needle) != 0; }

/* ---- test commands ---- */
static int g_echo_argc;
static char g_echo_join[256];

static int cmd_echo(shell_t *sh, int argc, char **argv)
{
    g_echo_argc = argc;
    g_echo_join[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(g_echo_join, " ");
        strcat(g_echo_join, argv[i]);
        shell_write(sh, argv[i]);
        shell_write(sh, i + 1 < argc ? " " : "\r\n");
    }
    return 0;
}

static int cmd_fail(shell_t *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    return -7;
}

static const shell_cmd_t g_cmds[] = {
    { "echo", "print arguments", cmd_echo },
    { "fail", "always fails",    cmd_fail },
};

static void feed_str(shell_t *sh, const char *s)
{
    for (; *s; s++)
        shell_feed(sh, *s);
}

/* ---- tokeniser ---- */
static void test_tokenize(void)
{
    printf("- tokenize: splitting, whitespace runs, argv overflow\n");
    char *argv[SHELL_MAX_ARGS];
    char b1[] = "load /sd/x.elf 3";
    CHECK(shell_tokenize(b1, argv, SHELL_MAX_ARGS) == 3, "three tokens");
    CHECK(strcmp(argv[0], "load") == 0 && strcmp(argv[2], "3") == 0, "argv values");

    char b2[] = "   spaced\t\targs   here   ";
    int n = shell_tokenize(b2, argv, SHELL_MAX_ARGS);
    CHECK(n == 3, "leading/trailing/tab runs collapse");
    CHECK(strcmp(argv[0], "spaced") == 0 && strcmp(argv[2], "here") == 0, "trim ok");

    char b3[] = "";
    CHECK(shell_tokenize(b3, argv, SHELL_MAX_ARGS) == 0, "empty -> 0 tokens");
    char b4[] = "     ";
    CHECK(shell_tokenize(b4, argv, SHELL_MAX_ARGS) == 0, "all spaces -> 0 tokens");

    char b5[] = "a b c d e f g h i j k l m n o p q r s";  /* 19 tokens */
    CHECK(shell_tokenize(b5, argv, SHELL_MAX_ARGS) == SHELL_MAX_ARGS,
          "argv overflow is clamped to max");
}

/* ---- dispatch + help ---- */
static void test_dispatch(void)
{
    printf("- dispatch: help, known, unknown, empty\n");
    shell_t sh;
    shell_init(&sh, g_cmds, 2, cap_out, 0);

    cap_reset();
    char h[] = "help"; char *av[SHELL_MAX_ARGS];
    int an = shell_tokenize(h, av, SHELL_MAX_ARGS);
    CHECK(shell_dispatch(&sh, an, av) == 0, "help returns 0");
    CHECK(saw("commands:") && saw("echo") && saw("fail") && saw("help"),
          "help lists every command incl. built-in");

    cap_reset();
    char e[] = "echo hi there"; char *ev[SHELL_MAX_ARGS];
    int en = shell_tokenize(e, ev, SHELL_MAX_ARGS);
    CHECK(shell_dispatch(&sh, en, ev) == 0, "echo dispatched");
    CHECK(g_echo_argc == 3 && strcmp(g_echo_join, "hi there") == 0,
          "handler saw the right argv");

    cap_reset();
    char u[] = "frobnicate"; char *uv[SHELL_MAX_ARGS];
    int un = shell_tokenize(u, uv, SHELL_MAX_ARGS);
    CHECK(shell_dispatch(&sh, un, uv) == SHELL_ENOCMD, "unknown -> ENOCMD");
    CHECK(saw("unknown command: frobnicate"), "unknown command message");

    cap_reset();
    CHECK(shell_dispatch(&sh, 0, uv) == SHELL_EMPTY, "empty -> EMPTY");

    cap_reset();
    char f[] = "fail"; char *fv[SHELL_MAX_ARGS];
    int fn = shell_tokenize(f, fv, SHELL_MAX_ARGS);
    CHECK(shell_dispatch(&sh, fn, fv) == -7, "handler error propagates");
    CHECK(saw("error: fail returned -7"), "error line rendered");
}

/* ---- line editor ---- */
static void test_editor(void)
{
    printf("- editor: echo, backspace, CR/LF/CRLF\n");
    shell_t sh;
    shell_init(&sh, g_cmds, 2, cap_out, 0);
    sh.prompt = "t> ";

    cap_reset();
    feed_str(&sh, "echo");
    CHECK(strcmp(g_out, "echo") == 0, "printable chars are echoed");

    /* backspace erases one char from buffer and screen */
    cap_reset();
    shell_feed(&sh, 'X');
    shell_feed(&sh, '\b');
    CHECK(strcmp(g_out, "X\b \b") == 0, "backspace emits erase sequence");
    /* buffer now back to "echo"; complete the line */
    cap_reset();
    shell_feed(&sh, '\r');
    CHECK(g_echo_argc == 1 && saw("t> "), "CR dispatches and reprints prompt");

    /* CRLF must dispatch once, not twice */
    g_echo_argc = -1;
    cap_reset();
    feed_str(&sh, "echo one\r\n");
    CHECK(g_echo_argc == 2, "CRLF dispatches exactly once");
    /* a lone LF still works as a terminator */
    g_echo_argc = -1;
    cap_reset();
    feed_str(&sh, "echo two\n");
    CHECK(g_echo_argc == 2, "bare LF also dispatches");

    /* empty line just reprints the prompt, dispatches nothing harmful */
    cap_reset();
    CHECK(shell_feed(&sh, '\n') == 1 && saw("t> "), "empty line -> prompt");
}

/* ---- overflow + fuzz ---- */
static void test_overflow(void)
{
    printf("- safety: over-long line and control bytes are contained\n");
    shell_t sh;
    shell_init(&sh, g_cmds, 2, cap_out, 0);
    cap_reset();
    for (int i = 0; i < 4000; i++)          /* 4000 'a' >> SHELL_LINE_MAX */
        shell_feed(&sh, 'a');
    CHECK(sh.len == SHELL_LINE_MAX - 1, "buffer capped at LINE_MAX-1");
    shell_feed(&sh, '\r');
    CHECK(sh.len == 0, "line reset after dispatch");

    /* control bytes are dropped, not buffered */
    cap_reset();
    shell_feed(&sh, 0x01);
    shell_feed(&sh, 0x1b);
    shell_feed(&sh, 0x09);      /* tab is a separator, dropped from mid-input */
    CHECK(sh.len == 0, "control bytes do not enter the buffer");
}

static void test_fuzz(void)
{
    printf("- fuzz: arbitrary byte stream never overflows or crashes\n");
    shell_t sh;
    shell_init(&sh, g_cmds, 2, cap_out, 0);
    uint32_t lcg = 0x1234567u;
    int bad = 0;
    for (int i = 0; i < 2000000; i++) {
        lcg = lcg * 1103515245u + 12345u;
        shell_feed(&sh, (char)(lcg >> 16));
        if (sh.len < 0 || sh.len >= SHELL_LINE_MAX)
            bad = 1;                         /* invariant: 0 <= len < MAX */
    }
    CHECK(!bad, "2,000,000 random bytes: buffer invariant held");
}

int main(void)
{
    printf("=== serial shell host tests (issue #80) ===\n");
    test_tokenize();
    test_dispatch();
    test_editor();
    test_overflow();
    test_fuzz();

    if (g_fail) {
        printf("SHELL TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("SHELL TESTS: ALL PASS\n");
    return 0;
}
