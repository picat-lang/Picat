/*
 * browser_stub.c -- browser-only glue, staged into build/stage/ by the
 * webasm Makefile.  Keeps emu/ untouched while providing the entry
 * points the web page drives the interpreter with.
 */

#include "bprolog.h"

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

    initialize_bprolog(4, argv);
}

int browser_rerun(void)
{
    use_gl_getline = 0;
    return bp_call_term(ADDTAG(insert_sym("$bp_first_call", 14, 0), ATM));
}
