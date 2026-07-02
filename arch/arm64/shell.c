/* arch/arm64/shell.c - serial control shell core (Issue #80, M13) */

#include "shell.h"

void shell_init(shell_t *sh, const shell_cmd_t *cmds, int n_cmds,
                void (*out)(void *io, const char *s), void *io)
{
    sh->cmds       = cmds;
    sh->n_cmds     = n_cmds;
    sh->out        = out;
    sh->io         = io;
    sh->ctx        = 0;
    sh->prompt     = 0;
    sh->len        = 0;
    sh->swallow_lf = 0;
    sh->line[0]    = '\0';
}

void shell_write(shell_t *sh, const char *s)
{
    if (sh->out)
        sh->out(sh->io, s);
}

void shell_prompt(shell_t *sh)
{
    shell_write(sh, sh->prompt ? sh->prompt : "> ");
}

int shell_tokenize(char *s, char **argv, int max)
{
    int argc = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t')     /* skip leading whitespace */
            *s++ = '\0';
        if (!*s)
            break;
        if (argc < max)
            argv[argc] = s;
        argc++;
        while (*s && *s != ' ' && *s != '\t')   /* consume the token */
            s++;
    }
    return (argc <= max) ? argc : max;
}

/* Render two columns for the built-in help without any printf. */
static void help_line(shell_t *sh, const char *name, const char *help)
{
    shell_write(sh, "  ");
    shell_write(sh, name);
    if (help && *help) {
        /* pad the name column to 12 chars */
        int n = 0;
        while (name[n])
            n++;
        for (int i = n; i < 12; i++)
            shell_write(sh, " ");
        shell_write(sh, help);
    }
    shell_write(sh, "\r\n");
}

static int cmd_help(shell_t *sh)
{
    shell_write(sh, "commands:\r\n");
    help_line(sh, "help", "list commands");
    for (int i = 0; i < sh->n_cmds; i++)
        help_line(sh, sh->cmds[i].name, sh->cmds[i].help);
    return 0;
}

/* Two-digit-friendly negative-int renderer for the error line (no printf). */
static void write_int(shell_t *sh, int v)
{
    char  buf[12];
    int   n = 0;
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0)
        shell_write(sh, "-");
    do {
        buf[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u);
    char out[13];
    int  k = 0;
    while (n)
        out[k++] = buf[--n];
    out[k] = '\0';
    shell_write(sh, out);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int shell_dispatch(shell_t *sh, int argc, char **argv)
{
    if (argc == 0)
        return SHELL_EMPTY;

    if (streq(argv[0], "help"))
        return cmd_help(sh);

    for (int i = 0; i < sh->n_cmds; i++) {
        if (streq(argv[0], sh->cmds[i].name)) {
            int r = sh->cmds[i].fn(sh, argc, argv);
            if (r < 0) {
                shell_write(sh, "error: ");
                shell_write(sh, sh->cmds[i].name);
                shell_write(sh, " returned ");
                write_int(sh, r);
                shell_write(sh, "\r\n");
            }
            return r;
        }
    }

    shell_write(sh, "unknown command: ");
    shell_write(sh, argv[0]);
    shell_write(sh, " (try 'help')\r\n");
    return SHELL_ENOCMD;
}

/* Tokenise the current buffer and dispatch it, then reset for the next line. */
static int run_line(shell_t *sh)
{
    char *argv[SHELL_MAX_ARGS];
    sh->line[sh->len] = '\0';
    int argc = shell_tokenize(sh->line, argv, SHELL_MAX_ARGS);
    int r = shell_dispatch(sh, argc, argv);
    sh->len = 0;
    shell_prompt(sh);
    return r;
}

int shell_feed(shell_t *sh, char c)
{
    /* Coalesce a CRLF pair: after a CR we swallow one following LF. */
    if (sh->swallow_lf) {
        sh->swallow_lf = 0;
        if (c == '\n')
            return 0;
    }

    if (c == '\r' || c == '\n') {
        if (c == '\r')
            sh->swallow_lf = 1;
        shell_write(sh, "\r\n");
        run_line(sh);
        return 1;
    }

    if (c == '\b' || c == 0x7f) {           /* backspace / DEL */
        if (sh->len > 0) {
            sh->len--;
            shell_write(sh, "\b \b");        /* erase the echoed char */
        }
        return 0;
    }

    /* Printable ASCII only; silently drop control bytes and overflow so no
     * input can corrupt the buffer or the terminal. */
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e)
        return 0;
    if (sh->len >= SHELL_LINE_MAX - 1)
        return 0;

    sh->line[sh->len++] = c;
    char echo[2] = { c, '\0' };
    shell_write(sh, echo);
    return 0;
}
