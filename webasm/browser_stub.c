/*
 * browser_stub.c -- browser-only glue, staged into build/stage/ by the
 * webasm Makefile.  Keeps emu/ untouched while providing the entry
 * points the web page drives the interpreter with.
 */

#include "bprolog.h"

#include <stdio.h>
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
 *   browser_rerun()         -- execute $bp_first_call again:
 *   compile/load /user_code.pi and call its main.  May be called
 *   repeatedly (initialize_bprolog is guarded, so only the boot
 *   call does any initialization, and init-loading the command
 *   line happens only at boot, when the runtime is fresh enough
 *   for it).
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

int browser_rerun(void)
{
    int rc;

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
