/*
 * browser_stub.c -- browser-only glue, staged into build/stage/ by the
 * webasm Makefile.  Keeps emu/ untouched while providing the entry
 * points the web page drives the interpreter with.
 */

#include "bprolog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* thread.c (pthreads) is not available in the browser: the thread()
 * and friends predicates are simply not registered. */
void Cboot_thread(void)
{
}

/*
 * Browser entry points (exported to JS):
 *
 *   browser_boot(picatpath) -- one-time runtime initialization with
 *   -p picatpath.  The command line carries a fixed program file,
 *   /user_code.pi, so every subsequent browser_rerun() executes
 *   whatever the page has written there.  No program is run yet.
 *
 *   browser_rerun(args)     -- execute $bp_first_call again:
 *   compile/load /user_code.pi and call its main (or the goal given
 *   with -g).  May be called repeatedly (initialize_bprolog is
 *   guarded, so only the boot call does any initialization).  The
 *   args string carries the rest of the native command line
 *   (picat [[-path Path] | [-p P] | [-s S] | [-b B] | [-g Goal] |
 *   [-c] | [-d] | [-log] | [--help] | [--version]]* File A1 A2 ...):
 *   tokens are whitespace-separated with ' and " grouping; the flags
 *   (and their values) are positioned BEFORE the fixed main file and
 *   the remaining non-flag tokens after it, exactly like the native
 *   CLI, so -g Goal runs Goal in place of main and trailing tokens
 *   reach main(A1,...,An) via command_line.  The command line is
 *   re-scanned by init_loading on every run (after the main_args
 *   list is reset, since add_main_arg prepends), which handles
 *   -c/-log/-g and the positionals per run; the memory-size flags
 *   -p/-s/-b are accepted and positioned but stay boot-time only.
 *   --help/--version print and stop; -c skips the run (browser_lint
 *   still follows, which is the compile+lint-only semantics).
 *
 * The page must keep the module line of /user_code.pi consistent
 * with the file name (rewrite it to "module user_code." if the
 * user's code declares any other module).
 */

void browser_boot(const char *picatpath)
{
    char *argv[5] = { (char *)"picatwasm", (char *)"-p",
                      (char *)picatpath, (char *)"/user_code.pi", 0 };

    /* /tmp must exist for the lint's stderr capture (err_hook.c's
       err_capture_stderr mkstemp()s a file under TMPDIR, default
       /tmp): the MEMFS may not have it, and without the capture the
       run's error messages would never reach browser_lint().  EEXIST
       is fine. */
    mkdir("/tmp", 0755);

    initialize_bprolog(4, argv);
}

/* CLI flags that take a value (the next argv token), per the native
   usage line.  -path/-p/-s/-b are boot-time only in the browser (the
   runtime is initialized once at browser_boot); they are accepted and
   positioned like the native CLI but have no per-run effect. */
static int br_takes_value(const char *t)
{
    return strcmp(t, "-path") == 0 || strcmp(t, "-p") == 0 ||
           strcmp(t, "-s") == 0 || strcmp(t, "-b") == 0 ||
           strcmp(t, "-g") == 0;
}

#define BR_MAX_TOKS 64

int browser_rerun(const char *args)
{
    char *tok[BR_MAX_TOKS];
    int consumed[BR_MAX_TOKS];
    char *argv[BR_MAX_TOKS + 3];
    int ntok = 0, argc = 0, i, rc, check_only = 0, show_help = 0;
    const char *p = args;

    for (i = 0; i < BR_MAX_TOKS; i++) consumed[i] = 0;

    /* tokenize: whitespace-separated; ' and " group (quotes stripped) */
    while (*p && ntok < BR_MAX_TOKS) {
        char buf[1024];
        int bl = 0, q = 0;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        while (*p && (q || (*p != ' ' && *p != '\t' &&
                            *p != '\n' && *p != '\r'))) {
            if (q && *p == q) { q = 0; p++; continue; }
            if (!q && (*p == '\'' || *p == '"')) { q = *p++; continue; }
            if (bl < (int)sizeof(buf) - 1) buf[bl++] = *p;
            p++;
        }
        buf[bl] = '\0';
        tok[ntok++] = strdup(buf);
    }

    /* pass 1: mark the flags (and the values they consume) */
    for (i = 0; i < ntok; i++) {
        if (tok[i][0] == '-' && tok[i][1] != '\0') {
            consumed[i] = 1;
            if (br_takes_value(tok[i]) && i + 1 < ntok)
                consumed[i + 1] = 1;
        }
    }

    for (i = 0; i < ntok; i++) {
        if (strcmp(tok[i], "--version") == 0) show_help = 1;
        else if (strcmp(tok[i], "--help") == 0) show_help = 2;
        else if (strcmp(tok[i], "-c") == 0) check_only = 1;
    }
    if (show_help) {   /* the native CLI prints and exits here */
        printf("Picat experimental, built on %s (commit %s), "
               "based on Picat 4.0\n", PICAT_BUILD_DATE,
               PICAT_BUILD_COMMIT);
        if (show_help == 2)
            printf("usage: picat [[-path Path] | [-p P] | [-s S] | [-b B] | "
                   "[-g Goal] | [-c] | [-d] | [-log] | [--help] | "
                   "[--version]]* PicatMainFileName A1 A2 ...\n");
        for (i = 0; i < ntok; i++) free(tok[i]);
        return 0;
    }

    /* rebuild the command line: flags (with their values, in order)
       before the fixed main file, the remaining tokens after it --
       the native CLI shape */
    argv[argc++] = (char *)"picatwasm";
    for (i = 0; i < ntok && argc < BR_MAX_TOKS + 1; i++)
        if (consumed[i]) argv[argc++] = tok[i];
    argv[argc++] = (char *)"/user_code.pi";
    for (i = 0; i < ntok && argc < BR_MAX_TOKS + 1; i++)
        if (!consumed[i]) argv[argc++] = tok[i];
    argv[argc] = 0;

    /* fresh command line for $bp_first_call: reset the main_args list
       (add_main_arg prepends) and re-run the init_loading arg scan,
       which handles -c/-log/-g and the positionals per run (and
       re-remembers the main file for the lint) */
    main_args = nil_sym;
    init_loading(argc, argv);

    for (i = 0; i < ntok; i++) free(tok[i]);

    if (check_only) {   /* lint only, no run: browser_lint (which the
                           page calls after browser_rerun) provides the
                           structural lint output; the undefined-name
                           check needs the run and is skipped */
        fflush(stdout);
        return 0;
    }

    use_gl_getline = 0;
    rc = bp_call_term(ADDTAG(insert_sym("$bp_first_call", 14, 0), ATM));
    /* layer 1 (of 2) of output: push the C stdio FILE buffer down to
       fd 1.  The emscripten tty buffer below fd 1 is drained from the
       JS side (see flushRunOutput in browser/index.html). */
    fflush(stdout);
    return rc;
}

/*
 * browser_lint() -- run the source-level lint (emu/err_hook.c) on
 * /user_code.pi after a run, exactly like the native path does from
 * main()'s tail: dump the captured stderr (the run's error messages,
 * which the capture holds back from the terminal until here), then
 * print the *** lint: line N: ... diagnostics.  The page calls this
 * after every browser_rerun and drains the output afterwards; the
 * lint resets itself for the next run.
 */
void browser_lint(void)
{
    extern int err_lint_main_file(void);
    extern void err_lint_reset(void);

    err_lint_main_file();
    err_lint_reset();
}

#ifdef SAT
/*
 * satext (the fork/exec external-solver layer, emu/satext.c) cannot
 * exist in a browser.  These stubs keep the built-in kissat path
 * (satext_ext_prepare() == 0 means "no external solver") and silence
 * the mirroring of clauses to an external CNF file.  The real
 * declarations are in emu/extern_decl.h.
 */
int satext_ext_prepare(void)
{
    return 0;   /* no external solver available */
}

int satext_ext_mirroring(void)
{
    return 0;
}

int satext_ext_run(void)
{
    return -1;  /* never reached: prepare says off */
}

int satext_ext_status(void)
{
    return 0;   /* unknown */
}

int satext_ext_model_value(int varnum)
{
    (void)varnum;
    return 0;
}

void satext_record_result(int st)
{
    (void)st;
}

void satext_clear_last_status(void)
{
}

void ext_cnf_reset(void)
{
}

void ext_cnf_set_mirroring(int on)
{
    (void)on;
}

void ext_cnf_push_lit(int32_t v)
{
    (void)v;
}

void ext_cnf_end_clause(void)
{
}

void Cboot_satext(void)
{
    /* external-solver predicates are not registered */
}
#endif
