/*
 * browser_stub.c -- stand-ins for functions whose real definitions live in
 * sources excluded from the browser build (see Makefile).  Registered via
 * the Makefile, staged into build/stage/ and compiled next to the picat
 * objects.  Kept in webasm/ so the emu/ tree stays untouched.
 */

/* thread.c (pthreads) is not available in the browser: the thread()
 * and friends predicates are simply not registered. */
void Cboot_thread(void)
{
}
