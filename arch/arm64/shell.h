/* arch/arm64/shell.h - serial control shell core (Issue #80, M13)
 *
 * The interactive front end to Tessera's control plane.  Today the graph is
 * driven by C code calling syscalls (issue #30) and patch files (issue #40);
 * this is the human interface - a line editor, a tokeniser, and a command
 * dispatcher over the UART, designed so the graph and patch commands of
 * issues #81/#82 register into it as ordinary table entries.
 *
 * The core is pure C and byte-oriented: the platform feeds it one input byte
 * at a time (shell_feed), so it works identically against a real PL011 RX
 * FIFO and a scripted test string, and every path is unit-tested on the host
 * (make test-arm-shell) including a fuzz sweep of arbitrary bytes.  It never
 * allocates and never blocks; the line buffer is fixed, so no input - however
 * long or malformed - can fault the kernel.
 *
 * Output goes through a caller-supplied sink.  On the target, the shell runs
 * off the audio core (CPU0 is untouchable) and shares the one UART with the
 * periodic stats reporter; the platform serialises whole messages with a lock
 * (see tests/arm64/virt/shell_main.c) so shell responses and `audio_latency:`
 * lines interleave without tearing.  The core itself takes no lock.
 */

#ifndef ARM64_SHELL_H
#define ARM64_SHELL_H

#define SHELL_LINE_MAX 128       /* longest command line (excess is dropped) */
#define SHELL_MAX_ARGS 16        /* argv capacity, including argv[0]          */

struct shell;

/* A command handler: argv[0] is the command name.  Returns 0 on success or a
 * negative code the shell renders as an error line.  Write output with
 * shell_write(); per-registration state is in sh->ctx. */
typedef int (*shell_fn)(struct shell *sh, int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;            /* one-line description, shown by `help`     */
    shell_fn    fn;
} shell_cmd_t;

typedef struct shell {
    const shell_cmd_t *cmds;
    int                n_cmds;
    void             (*out)(void *io, const char *s);   /* output sink        */
    void              *io;
    void              *ctx;      /* opaque state for command handlers          */
    const char        *prompt;   /* e.g. "tessera> " (NULL -> "> ")           */
    char               line[SHELL_LINE_MAX];
    int                len;
    int                swallow_lf;  /* coalesce a CR's trailing LF            */
} shell_t;

/* Bind a command table and an output sink.  `cmds` must outlive `sh`. */
void shell_init(shell_t *sh, const shell_cmd_t *cmds, int n_cmds,
                void (*out)(void *io, const char *s), void *io);

/* Emit the prompt (call once at start-up; the shell reprints it after each
 * dispatched line). */
void shell_prompt(shell_t *sh);

/* Write a string through the sink (for command handlers). */
void shell_write(shell_t *sh, const char *s);

/* Feed one input byte: echoes printable characters, handles backspace, and on
 * end-of-line tokenises the buffer and dispatches.  Returns 1 if a line was
 * dispatched (including an empty one), 0 otherwise. */
int shell_feed(shell_t *sh, char c);

/* Split `s` in place into argv on runs of spaces/tabs (writing NULs); returns
 * argc (0..max).  Exposed for testing. */
int shell_tokenize(char *s, char **argv, int max);

/* Look up, then run, a tokenised command (used by shell_feed).  `help` is
 * always available and lists every registered command.  Returns the handler's
 * value, or a negative shell code for an unknown/empty command. */
int shell_dispatch(shell_t *sh, int argc, char **argv);

/* shell_dispatch / shell_feed return codes (negative) not from a handler. */
#define SHELL_EMPTY    (-1)      /* blank line: prompt reprinted, no command  */
#define SHELL_ENOCMD   (-2)      /* no such command                           */

#endif /* ARM64_SHELL_H */
