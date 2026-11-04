#include <stdlib.h>
#include <time.h>
#include "basic.h"
#include  <stdio.h>
extern char *string_in;


/*
  #ifndef lint
  static char     rcsid[] =
  "$Id: getline.c,v 3.14 1994/09/26 22:56:19 thewalt Exp $";
  static char    *copyright = "Copyright (C) 1991-1994, Chris Thewalt";
  #endif
*/
/*
 * Copyright (C) 1991-1994 by Chris Thewalt (thewalt@ce.berkeley.edu)
 *
 * Permission to use, copy, modify, and distribute this software 
 * for any purpose and without fee is hereby granted, provided
 * that the above copyright notices appear in all copies and that both the
 * copyright notice and this permission notice appear in supporting
 * documentation.  This software is provided "as is" without express or
 * implied warranty.
 *
 * Thanks to the following people who have provided enhancements and fixes:
 *   Ron Ueberschaer, Christoph Keller, Scott Schwartz, Steven List,
 *   DaviD W. Sanderson, Goran Bostrom, Michael Gleason, Glenn Kasten,
 *   Edin Hodzic, Eric J Bivona, Kai Uwe Rommel, Danny Quah, Ulrich Betzler,
 *   V. Phaniraj.
 */

#include       "getline.h"
static int gl_tab(char *buf, int offset, int *loc);  /* forward reference needed for gl_tab_hook */

/******************** imported interface *********************************/

#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>

#if defined(WIN32) && defined(M64BITS)
#define sys_write(a, b, c) _write(a, b, c)
#define sys_isatty(t) _isatty(t)
#else
#include <unistd.h>
#define sys_write(a, b, c) write(a, b, c)
#define sys_isatty(t) isatty(t)
#endif

/* fire-and-forget console writes: the return value is deliberately not
   used, which would otherwise trip -Wunused-result */
static void write_ignore(int fd, const void *buf, size_t len)
{
    ssize_t w = sys_write(fd, buf, len);
    (void)w;
}



/********************* exported interface ********************************/

void gl_setwidth(int);  /* specify width of screen */
void gl_histadd(char *);  /* adds entries to hist */
void gl_strwidth(size_t (*)(const char *));  /* to bind gl_strlen */

int (*gl_in_hook)(char *) = 0;
int (*gl_out_hook)(char *) = 0;
int (*gl_tab_hook)(char *, int, int *) = gl_tab;

/******************** internal interface *********************************/

#define BUF_SIZE 1000

static int gl_init_done = -1;  /* terminal mode flag  */
static int gl_termw = 180;  /* actual terminal width */
static int gl_scroll = 27;  /* width of EOL scrolling region */
static int gl_width = 0;  /* net size available for input */
static int gl_extent = 0;  /* how far to redraw, 0 means all */
static int gl_overwrite = 0;  /* overwrite mode */
static int gl_pos, gl_cnt = 0;  /* position and size of input */
static char gl_buf[BUF_SIZE];  /* input buffer */
static char gl_killbuf[BUF_SIZE] = "";  /* killed text */
static char *gl_prompt;  /* to save the prompt string */
static char gl_intrc = 0;  /* keyboard SIGINT char */
static char gl_quitc = 0;  /* keyboard SIGQUIT char */
static char gl_suspc = 0;  /* keyboard SIGTSTP char */
static char gl_dsuspc = 0;  /* delayed SIGTSTP char */
static int gl_search_mode = 0;  /* search mode flag */

static void gl_init();  /* prepare to edit a line */
static void gl_cleanup();  /* to undo gl_init */
static void gl_char_init();  /* get ready for no echo input */
static void gl_char_cleanup();  /* undo gl_char_init */
static size_t gl_u8_strwidth(const char *);  /* display width of a string */
static size_t (*gl_strlen)(const char *) = gl_u8_strwidth;
/* returns printable prompt width */

static void gl_addchar(int);  /* install specified char */
static void gl_del(int);  /* del, either left (-1) or cur (0) */
static void gl_error(char *);  /* write error msg and die */
static void gl_fixup(char *, int, int);  /* fixup state variables and screen */
static int gl_getc();  /* read one char from terminal */
static void gl_kill(int);  /* delete to EOL */
static void gl_newline();  /* handle \n or \r */
static void gl_putc(int);  /* write one char to terminal */
static void gl_puts(char *);  /* write a line to terminal */
static void gl_redraw();  /* issue \n and redraw all */
static void gl_transpose();  /* transpose two chars */
static void gl_yank();  /* yank killed text */
static void gl_word(int);  /* move a word */

static void hist_init();  /* initializes hist pointers */
static char *hist_next();  /* return ptr to next item */
static char *hist_prev();  /* return ptr to prev item */
static char *hist_save(char *);  /* makes copy of a string, without NL */

static void search_addchar(int);  /* increment search string */
static void search_term();  /* reset with current contents */
static void search_back(int);  /* look back for current string */
static void search_forw(int);  /* look forw for current string */

/*
 * UTF-8 support.
 *
 * gl_buf holds UTF-8 encoded bytes; gl_pos and gl_cnt are byte offsets that
 * always sit on a character boundary.  Cursor movement, deletion and
 * transposition work on whole characters (a base character together with
 * any zero-width combining marks that follow it), and all screen layout is
 * done in display columns rather than bytes, so double-width (CJK) and
 * zero-width characters are positioned correctly.
 */
static int gl_u8_seqlen(int c);  /* length of sequence from lead byte */
static int gl_u8_decode(const char *, int, int *);  /* decode one char */
static int gl_u8_width(int);  /* display width of a code point */
static int gl_u8_colwidth(const char *, int, int);  /* width of a byte range */
static int gl_u8_next(int);  /* boundary of next char in gl_buf */
static int gl_u8_prev(int);  /* boundary of previous char in gl_buf */
static int gl_u8_snap(int);  /* round a byte offset down to a boundary */
static void gl_addbytes(const char *, int);  /* insert one encoded char */
static void search_addstr(const char *, int);  /* add one char to search */

/************************ nonportable part *********************************/

/* extern int      write(); */
#ifdef unix
#ifndef __unix__
#define __unix__
#endif  /* not __unix__ */
#endif  /* unix */

#ifdef DARWIN
#define __unix__
#define POSIX
#endif

#ifdef _IBMR2
#ifndef __unix__
#define __unix__
#endif
#endif

#ifdef WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifdef __GO32__
#include <pc.h>
#undef MSDOS
#undef __unix__
#endif

#ifdef MSDOS
#include <bios.h>
#endif


#ifdef __unix__
#include <unistd.h>
#ifdef POSIX  /* use POSIX interface */
#include <termios.h>
struct termios new_termios, old_termios;
#else  /* not POSIX */
#include <sys/ioctl.h>
#ifdef M_XENIX  /* does not really use bsd terminal interface */
#undef BSD
#endif  /* M_XENIX */
#ifdef BSD  /* use BSD interface */
#include <sgtty.h>
struct sgttyb new_tty, old_tty;
struct tchars tch;
struct ltchars ltch;
#else  /* use SYSV interface */
#include <termio.h>
struct termio new_termio, old_termio;
#endif  /* BSD */
#endif  /* POSIX */
#endif  /* __unix__ */

#ifdef vms
#include <descrip.h>
#include <ttdef.h>
#include <iodef.h>
#include unixio

static int setbuff[2];  /* buffer to set terminal attributes */
static short chan = -1;  /* channel to terminal */
struct dsc$descriptor_s descrip;  /* VMS descriptor */
#endif

#ifdef WIN32
static unsigned int gl_old_output_cp = 0;
static DWORD gl_old_input_mode;
static int gl_input_mode_saved = 0;

static void gl_win_restore_output_cp(void)
{
    /* Flush UTF-8 output before returning the shared console to its owner. */
    fflush(stdout);
    fflush(stderr);
    if (gl_old_output_cp != 0)
        SetConsoleOutputCP(gl_old_output_cp);
}

/* Read Unicode text independently of the special-key encoding used by
 * _getwch(), whose 0xE0 prefix collides with the character U+00E0 (à).
 * Return UTF-16 code units, or the editor's ASCII navigation commands.
 */
static int gl_win_getc(void)
{
    static KEY_EVENT_RECORD key;
    static WORD repeats = 0;
    INPUT_RECORD record;
    DWORD count;
    int c;

    for (;;) {
        if (repeats != 0) {
            repeats--;
        } else {
            if (!ReadConsoleInputW(GetStdHandle(STD_INPUT_HANDLE),
                                   &record, 1, &count) || count == 0)
                return -1;
            if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
                continue;
            key = record.Event.KeyEvent;
            repeats = key.wRepeatCount > 0 ? key.wRepeatCount - 1 : 0;
        }
        /* Text takes precedence, including AltGr and Alt-code input. */
        if (key.uChar.UnicodeChar != 0)
            return key.uChar.UnicodeChar;
        switch (key.wVirtualKeyCode) {
        case VK_UP:     c = 16; break;
        case VK_DOWN:   c = 14; break;
        case VK_LEFT:   c = 2;  break;
        case VK_RIGHT:  c = 6;  break;
        case VK_DELETE: c = 4;  break;
        case VK_HOME:   c = 1;  break;
        case VK_END:    c = 5;  break;
        case VK_INSERT: c = 15; break;
        default: continue;  /* modifiers, function keys, non-text events */
        }
        return c;
    }
}
#endif

static void
gl_char_init()  /* turn off input echo */
{
#ifdef WIN32
    /* Query results are UTF-8 too. Keep UTF-8 active between prompts and
     * restore the original code page only when Picat exits. */
    if (gl_old_output_cp == 0) {
        gl_old_output_cp = GetConsoleOutputCP();
        if (gl_old_output_cp != 0)
            atexit(gl_win_restore_output_cp);
    }
    fflush(stdout);
    fflush(stderr);
    SetConsoleOutputCP(65001);  /* CP_UTF8 */
    gl_input_mode_saved = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),
                                         &gl_old_input_mode);
    if (gl_input_mode_saved)
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), gl_old_input_mode &
                       ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
#endif
#ifdef __unix__
#ifdef POSIX
    tcgetattr(0, &old_termios);
    gl_intrc = old_termios.c_cc[VINTR];
    gl_quitc = old_termios.c_cc[VQUIT];
#ifdef VSUSP
    gl_suspc = old_termios.c_cc[VSUSP];
#endif
#ifdef VDSUSP
    gl_dsuspc = old_termios.c_cc[VDSUSP];
#endif
    new_termios = old_termios;
    new_termios.c_iflag &= ~(BRKINT|ISTRIP|IXON|IXOFF);
    new_termios.c_iflag |= (IGNBRK|IGNPAR);
    new_termios.c_lflag &= ~(ICANON|ISIG|IEXTEN|ECHO);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &new_termios);
#else  /* not POSIX */
#ifdef BSD  /* BSD */
    ioctl(0, TIOCGETC, &tch);
    ioctl(0, TIOCGLTC, &ltch);
    gl_intrc = tch.t_intrc;
    gl_quitc = tch.t_quitc;
    gl_suspc = ltch.t_suspc;
    gl_dsuspc = ltch.t_dsuspc;
    ioctl(0, TIOCGETP, &old_tty);
    new_tty = old_tty;
    new_tty.sg_flags |= RAW;
    new_tty.sg_flags &= ~ECHO;
    ioctl(0, TIOCSETN, &new_tty);
#else  /* SYSV */
    ioctl(0, TCGETA, &old_termio);
    gl_intrc = old_termio.c_cc[VINTR];
    gl_quitc = old_termio.c_cc[VQUIT];
    new_termio = old_termio;
    new_termio.c_iflag &= ~(BRKINT|ISTRIP|IXON|IXOFF);
    new_termio.c_iflag |= (IGNBRK|IGNPAR);
    new_termio.c_lflag &= ~(ICANON|ISIG|ECHO);
    new_termio.c_cc[VMIN] = 1;
    new_termio.c_cc[VTIME] = 0;
    ioctl(0, TCSETA, &new_termio);
#endif  /* BSD */
#endif  /* POSIX */
#endif  /* __unix__ */

#ifdef vms
    descrip.dsc$w_length = strlen("tt:");
    descrip.dsc$b_dtype = DSC$K_DTYPE_T;
    descrip.dsc$b_class = DSC$K_CLASS_S;
    descrip.dsc$a_pointer = "tt:";
    (void)sys$assign(&descrip, &chan, 0, 0);
    (void)sys$qiow(0, chan, IO$_SENSEMODE, 0, 0, 0, setbuff, 8, 0, 0, 0, 0);
    setbuff[1] |= TT$M_NOECHO;
    (void)sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, setbuff, 8, 0, 0, 0, 0);
#endif  /* vms */
}

static void
gl_char_cleanup()  /* undo effects of gl_char_init */
{
#ifdef WIN32
    if (gl_input_mode_saved) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), gl_old_input_mode);
        gl_input_mode_saved = 0;
    }
#endif
#ifdef __unix__
#ifdef POSIX
    tcsetattr(0, TCSANOW, &old_termios);
#else  /* not POSIX */
#ifdef BSD  /* BSD */
    ioctl(0, TIOCSETN, &old_tty);
#else  /* SYSV */
    ioctl(0, TCSETA, &old_termio);
#endif  /* BSD */
#endif  /* POSIX */
#endif  /* __unix__ */

#ifdef vms
    setbuff[1] &= ~TT$M_NOECHO;
    (void)sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, setbuff, 8, 0, 0, 0, 0);
    sys$dassgn(chan);
    chan = -1;
#endif
}

#if MSDOS || __EMX__ || __GO32__ || WIN32
int pc_keymap(c)
    int c;
{
    switch (c) {
    case 72: c = 16;  /* up -> ^P */
        break;
    case 80: c = 14;  /* down -> ^N */
        break;
    case 75: c = 2;  /* left -> ^B */
        break;
    case 77: c = 6;  /* right -> ^F */
        break;
    case 83: c = 4;  /* delete -> ^D */
        break;
    case 71: c = 1;  /* home -> ^A */
        break;
    case 79: c = 5;  /* end -> ^E */
        break;
    case 82: c = 15;  /* insert -> ^O */
        break;
    default: c = 0;  /* make it garbage */
    }
    return c;
}
#endif  /* MSDOS || __EMX__ || __GO32__ || WIN32 */

static int
gl_getc()
/* get a character without echoing it to screen */
{
    int c;
#ifdef __unix__
    unsigned char ch;
#endif
#ifdef WIN32
    /* UTF-8 bytes still to be returned for the last wide char read */
    static unsigned char pending[4];
    static int npending = 0, ipending = 0;
    static int saved_wc = -1;
    int wc, wc2;

    if (ipending < npending)
        return pending[ipending++];
#endif

#ifdef __unix__
    while ((c = read(0, &ch, 1)) == -1) {
        if (errno != EINTR)
            break;
    }
    c = (c <= 0)? -1 : (int)ch;  /* 0..255, never negative */
#endif  /* __unix__ */
#ifdef MSDOS
    c = _bios_keybrd(_NKEYBRD_READ);
#endif  /* MSDOS */
#ifdef __GO32__
    c = getkey () ;
    if (c > 255) c = pc_keymap(c & 0377);
#endif  /* __GO32__ */
#ifdef __TURBOC__
    while(!bioskey(1))
        ;
    c = bioskey(0);
#endif
#if MSDOS || __TURBOC__
    if ((c & 0377) == 224) {
        c = pc_keymap((c >> 8) & 0377);
    } else {
        c &= 0377;
    }
#endif  /* MSDOS || __TURBOC__ */
#ifdef __EMX__
    c = _read_kbd(0, 1, 0);
    if (c == 224 || c == 0) {
        c = pc_keymap(_read_kbd(0, 1, 0));
    } else {
        c &= 0377;
    }
#endif
#ifdef WIN32
    /* Encode console UTF-16 as UTF-8, one byte per call. */
    wc = saved_wc >= 0 ? saved_wc : gl_win_getc();
    saved_wc = -1;
    if (wc < 0x80) {
        c = wc;
    } else {
        if (wc >= 0xD800 && wc <= 0xDBFF) {  /* surrogate pair */
            wc2 = gl_win_getc();
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF)
                wc = 0x10000 + ((wc - 0xD800) << 10) + (wc2 - 0xDC00);
            else {
                wc = 0xFFFD;
                /* Do not lose the character following an unpaired surrogate. */
                saved_wc = wc2;
            }
        } else if (wc >= 0xDC00 && wc <= 0xDFFF) {
            wc = 0xFFFD;  /* unpaired low surrogate */
        }
        npending = 0;
        if (wc < 0x800) {
            pending[npending++] = 0xC0 | (wc >> 6);
        } else if (wc < 0x10000) {
            pending[npending++] = 0xE0 | (wc >> 12);
            pending[npending++] = 0x80 | ((wc >> 6) & 0x3F);
        } else {
            pending[npending++] = 0xF0 | (wc >> 18);
            pending[npending++] = 0x80 | ((wc >> 12) & 0x3F);
            pending[npending++] = 0x80 | ((wc >> 6) & 0x3F);
        }
        pending[npending++] = 0x80 | (wc & 0x3F);
        ipending = 1;
        c = pending[0];
    }
#endif
#ifdef vms
    if(chan < 0) {
        c = '\0';
    }
    (void)sys$qiow(0, chan, IO$_TTYREADALL, 0, 0, 0, &c, 1, 0, 0, 0, 0);
    c &= 0177;  /* get a char */
#endif
    return c;
}

static void
gl_putc(c)
    int c;
{
    char ch = c;

    if (ch == '\r' || ch == '\n'); else write_ignore(1, &ch, 1);

    /*    if (ch == '\n') {
          ch = '\r'; 
          write(1, &ch, 1);
          }
    */
}

/******************** fairly portable part *********************************/

static void
gl_puts(buf)
    char *buf;
{
    int len;

    if (buf) {
        len = strlen(buf);
        write_ignore(1, buf, len);
    }
}

static void
gl_error(buf)
    char *buf;
{
    int len = strlen(buf);

    gl_cleanup();
    write_ignore(2, buf, len);
    exit(1);
}

static void
gl_init()
/* set up variables and terminal */
{
    if (gl_init_done < 0) {  /* -1 only on startup */
        hist_init();
    }
    if (sys_isatty(0) == 0 || sys_isatty(1) == 0)
        gl_error("\n*** Error: getline(): not interactive, use stdio.\n");
    gl_char_init();
    gl_init_done = 1;
}

static void
gl_cleanup()
/* undo effects of gl_init, as necessary */
{
    if (gl_init_done > 0)
        gl_char_cleanup();
    gl_init_done = 0;
}

void
gl_setwidth(w)
    int w;
{
    if (w > 20) {
        gl_termw = w;
        gl_scroll = w / 3;
    } else {
        gl_error("\n*** Error: minimum screen width is 21\n");
    }
}

char *
bp_getline(prompt)
    char *prompt;
{
    int c, loc, tmp, n, i;
    char seq[4];

#ifdef __unix__
    int sig;
#endif

    gl_init();
    gl_prompt = (prompt)? prompt : "";
    gl_buf[0] = 0;
    if (gl_in_hook)
        gl_in_hook(gl_buf);
    gl_fixup(gl_prompt, -2, BUF_SIZE);
    while ((c = gl_getc()) >= 0) {
        gl_extent = 0;  /* reset to full extent */
        if (c >= 0x80) {  /* first byte of a UTF-8 encoded character */
            n = gl_u8_seqlen(c);
            seq[0] = (char)c;
            for (i = 1; i < n; i++) {
                c = gl_getc();
                if (c < 0x80 || c > 0xBF)  /* not a continuation byte */
                    break;
                seq[i] = (char)c;
            }
            if (n > 0 && i == n && gl_u8_decode(seq, n, &tmp) == n) {
                if (gl_search_mode)
                    search_addstr(seq, n);
                else
                    gl_addbytes(seq, n);
            } else {
                gl_putc('\007');  /* malformed UTF-8, drop it */
            }
            if (c < 0)
                break;  /* EOF in the middle of a sequence */
        } else if (isprint(c)) {
            if (gl_search_mode)
                search_addchar(c);
            else
                gl_addchar(c);
        } else {
            if (gl_search_mode) {
                if (c == '\033' || c == '\016' || c == '\020') {
                    search_term();
                    c = 0;  /* ignore the character */
                } else if (c == '\010' || c == '\177') {
                    search_addchar(-1);  /* unwind search string */
                    c = 0;
                } else if (c != '\022' && c != '\023') {
                    search_term();  /* terminate and handle char */
                }
            }
            switch (c) {
            case '\n': case '\r':  /* newline */
                gl_newline();
                gl_cleanup();
                return gl_buf;
                /*NOTREACHED*/
                break;
            case '\001': gl_fixup(gl_prompt, -1, 0);  /* ^A */
                break;
            case '\002':  /* ^B */
                gl_fixup(gl_prompt, -1, (gl_pos > 0)? gl_u8_prev(gl_pos) : -1);
                break;
            case '\004':  /* ^D */
                if (gl_cnt == 0) {
                    gl_buf[0] = 0;
                    gl_cleanup();
                    printf("\n");
                    exit(0);
                } else {
                    gl_del(0);
                }
                break;
            case '\005': gl_fixup(gl_prompt, -1, gl_cnt);  /* ^E */
                break;
            case '\006':  /* ^F */
                gl_fixup(gl_prompt, -1, (gl_pos < gl_cnt)? gl_u8_next(gl_pos) : gl_cnt+1);
                break;
            case '\010': case '\177': gl_del(-1);  /* ^H and DEL */
                break;
            case '\t':  /* TAB */
                if (gl_tab_hook) {
                    tmp = gl_pos;
                    loc = gl_tab_hook(gl_buf, gl_strlen(gl_prompt), &tmp);
                    if (loc >= 0 || tmp != gl_pos)
                        gl_fixup(gl_prompt, loc, tmp);
                }
                break;
            case '\013': gl_kill(gl_pos);  /* ^K */
                break;
            case '\014': gl_redraw();  /* ^L */
                break;
            case '\016':  /* ^N */
                strcpy(gl_buf, hist_next());
                if (gl_in_hook)
                    gl_in_hook(gl_buf);
                gl_fixup(gl_prompt, 0, BUF_SIZE);
                break;
            case '\017': gl_overwrite = !gl_overwrite;  /* ^O */
                break;
            case '\020':  /* ^P */
                strcpy(gl_buf, hist_prev());
                if (gl_in_hook)
                    gl_in_hook(gl_buf);
                gl_fixup(gl_prompt, 0, BUF_SIZE);
                break;
            case '\022': search_back(1);  /* ^R */
                break;
            case '\023': search_forw(1);  /* ^S */
                break;
            case '\024': gl_transpose();  /* ^T */
                break;
            case '\025': gl_kill(0);  /* ^U */
                break;
            case '\031': gl_yank();  /* ^Y */
                break;
            case '\033':  /* ansi arrow keys */
                c = gl_getc();
                if (c == '[') {
                    switch(c = gl_getc()) {
                    case 'A':  /* up */
                        strcpy(gl_buf, hist_prev());
                        if (gl_in_hook)
                            gl_in_hook(gl_buf);
                        gl_fixup(gl_prompt, 0, BUF_SIZE);
                        break;
                    case 'B':  /* down */
                        strcpy(gl_buf, hist_next());
                        if (gl_in_hook)
                            gl_in_hook(gl_buf);
                        gl_fixup(gl_prompt, 0, BUF_SIZE);
                        break;
                    case 'C':  /* right */
                        gl_fixup(gl_prompt, -1, (gl_pos < gl_cnt)? gl_u8_next(gl_pos) : gl_cnt+1);
                        break;
                    case 'D':  /* left */
                        gl_fixup(gl_prompt, -1, (gl_pos > 0)? gl_u8_prev(gl_pos) : -1);
                        break;
                    default: gl_putc('\007');  /* who knows */
                        break;
                    }
                } else if (c == 'f' || c == 'F') {
                    gl_word(1);
                } else if (c == 'b' || c == 'B') {
                    gl_word(-1);
                } else
                    gl_putc('\007');
                break;
            default:  /* check for a terminal signal */
#ifdef __unix__
                if (c > 0) {  /* ignore 0 (reset above) */
                    sig = 0;
#ifdef SIGINT
                    if (c == gl_intrc)
                        sig = SIGINT;
#endif
#ifdef SIGQUIT
                    if (c == gl_quitc)
                        sig = SIGQUIT;
#endif
#ifdef SIGTSTP
                    if (c == gl_suspc || c == gl_dsuspc)
                        sig = SIGTSTP;
#endif
                    if (sig != 0) {
                        gl_cleanup();
                        kill(0, sig);
                        gl_init();
                        gl_redraw();
                        c = 0;
                    }
                }
#endif  /* __unix__ */
                if (c > 0)
                    gl_putc('\007');
                break;
            }
        }
    }
    gl_cleanup();
    gl_buf[0] = 0;
    return gl_buf;
}

static void
gl_addchar(c)
    int c;
    /* adds the (ASCII) character c to the input buffer at current location */
{
    char ch = (char)c;

    gl_addbytes(&ch, 1);
}

static void
gl_addbytes(s, n)
    const char *s;
    int n;
    /* adds one UTF-8 encoded character (n bytes) at current location */
{
    int i, old = 0;

    if (gl_overwrite && gl_pos < gl_cnt)
        old = gl_u8_next(gl_pos) - gl_pos;  /* bytes of char replaced */
    if (gl_cnt - old + n >= BUF_SIZE - 1)
        gl_error("\n*** Error: getline(): input buffer overflow\n");
    if (n != old)
        memmove(gl_buf + gl_pos + n, gl_buf + gl_pos + old,
                gl_cnt - gl_pos - old + 1);  /* includes the NUL */
    for (i = 0; i < n; i++)
        gl_buf[gl_pos + i] = s[i];
    gl_fixup(gl_prompt, gl_pos, gl_pos + n);
}

static void
gl_yank()
/* adds the kill buffer to the input buffer at current location */
{
    int i, len;

    len = strlen(gl_killbuf);
    if (len > 0) {
        if (gl_overwrite == 0) {
            if (gl_cnt + len >= BUF_SIZE - 1)
                gl_error("\n*** Error: getline(): input buffer overflow\n");
            for (i = gl_cnt; i >= gl_pos; i--)
                gl_buf[i+len] = gl_buf[i];
            for (i = 0; i < len; i++)
                gl_buf[gl_pos+i] = gl_killbuf[i];
            gl_fixup(gl_prompt, gl_pos, gl_pos+len);
        } else {
            if (gl_pos + len > gl_cnt) {
                if (gl_pos + len >= BUF_SIZE - 1)
                    gl_error("\n*** Error: getline(): input buffer overflow\n");
                gl_buf[gl_pos + len] = 0;
            }
            for (i = 0; i < len; i++)
                gl_buf[gl_pos+i] = gl_killbuf[i];
            gl_extent = len;
            gl_fixup(gl_prompt, gl_pos, gl_pos+len);
        }
    } else
        gl_putc('\007');
}

static void
gl_transpose()
/* switch character under cursor and to left of cursor */
{
    char tmp[BUF_SIZE];
    int a, b, la, lb;

    if (gl_pos > 0 && gl_cnt > gl_pos) {
        a = gl_u8_prev(gl_pos);  /* char left of cursor: [a, gl_pos) */
        b = gl_u8_next(gl_pos);  /* char under cursor:   [gl_pos, b) */
        la = gl_pos - a;
        lb = b - gl_pos;
        memcpy(tmp, gl_buf + a, la);
        memmove(gl_buf + a, gl_buf + gl_pos, lb);
        memcpy(gl_buf + a + lb, tmp, la);
        gl_extent = 2;
        gl_fixup(gl_prompt, a, a + lb);
    } else
        gl_putc('\007');
}

static void
gl_newline()
/*
 * Cleans up entire line before returning to caller. A \n is appended.
 * If line longer than screen, we redraw starting at beginning
 */
{
    int change = gl_cnt;
    int len = gl_cnt;
    int loc = gl_width - 5;  /* shifts line back to start position */

    if (gl_cnt >= BUF_SIZE - 1)
        gl_error("\n*** Error: getline(): input buffer overflow\n");
    if (gl_out_hook) {
        change = gl_out_hook(gl_buf);
        len = strlen(gl_buf);
    }
    if (loc > len)
        loc = len;
    gl_fixup(gl_prompt, change, loc);  /* must do this before appending \n */
    gl_buf[len] = '\n';
    gl_buf[len+1] = '\0';
    gl_putc('\n');
}

static void
gl_del(loc)
    int loc;
    /*
     * Delete a character.  The loc variable can be:
     *    -1 : delete character to left of cursor
     *     0 : delete character under cursor
     */
{
    int from, to;

    if ((loc == -1 && gl_pos > 0) || (loc == 0 && gl_pos < gl_cnt)) {
        if (loc == -1) {
            from = gl_u8_prev(gl_pos);
            to = gl_pos;
        } else {
            from = gl_pos;
            to = gl_u8_next(gl_pos);
        }
        memmove(gl_buf + from, gl_buf + to, gl_cnt - to + 1);
        gl_fixup(gl_prompt, from, from);
    } else
        gl_putc('\007');
}

static void
gl_kill(pos)
    int pos;
    /* delete from pos to the end of line */
{
    if (pos < gl_cnt) {
        strcpy(gl_killbuf, gl_buf + pos);
        gl_buf[pos] = '\0';
        gl_fixup(gl_prompt, pos, pos);
    } else
        gl_putc('\007');
}

static void
gl_word(direction)
    int direction;
    /* move forward or backword one word */
{
    int pos = gl_pos;

    if (direction > 0) {  /* forward */
        while (!isspace((unsigned char)gl_buf[pos]) && pos < gl_cnt)
            pos++;
        while (isspace((unsigned char)gl_buf[pos]) && pos < gl_cnt)
            pos++;
    } else {  /* backword */
        if (pos > 0)
            pos--;
        while (isspace((unsigned char)gl_buf[pos]) && pos > 0)
            pos--;
        while (!isspace((unsigned char)gl_buf[pos]) && pos > 0)
            pos--;
        if (pos < gl_cnt && isspace((unsigned char)gl_buf[pos]))  /* move onto word */
            pos++;
    }
    gl_fixup(gl_prompt, -1, pos);
}

static void
gl_redraw()
/* emit a newline, reset and redraw prompt and current input line */
{
    if (gl_init_done > 0) {
        gl_putc('\n');
        gl_fixup(gl_prompt, -2, gl_pos);
    }
}

static void
gl_fixup(prompt, change, cursor)
    char *prompt;
    int change, cursor;
/*
 * This function is used both for redrawing when input changes or for
 * moving within the input line.  The parameters are:
 *   prompt:  compared to last_prompt[] for changes;
 *   change : the index of the start of changes in the input buffer,
 *            with -1 indicating no changes, -2 indicating we're on
 *            a new line, redraw everything.
 *   cursor : the desired location of the cursor after the call.
 *            A value of BUF_SIZE can be used  to indicate the cursor should
 *            move just past the end of the input line.
 *
 * All indices are byte offsets into gl_buf.  Screen positions are computed
 * in display columns, so multibyte and double-width characters are drawn
 * correctly.  When the line is wider than the terminal it scrolls
 * horizontally; a '$' marks text hidden off either edge.
 */
{
    static int gl_shift;  /* byte index of first on-screen character */
    static int shift_col;  /* display column of gl_shift */
    static int off_right;  /* true if more text right of screen */
    static int off_left;  /* true if more text left of screen */
    static int scr_col;  /* terminal cursor column, relative to the
                            start of the input area */
    static int drawn;  /* columns used on screen by the input area */
    static char last_prompt[BUF_SIZE] = "";
    int redraw = 0;  /* must the input area be redrawn? */
    int prompt_redraw = 0;  /* must the prompt be redrawn too? */
    int old_prompt_w = 0;
    int cursor_col, new_shift_col, new_shift, col, w, cp, n;
    int limit, i, p;

    if (change == -2) {  /* reset */
        gl_pos = gl_cnt = gl_shift = shift_col = 0;
        off_right = off_left = scr_col = drawn = 0;
        gl_putc('\r');
        gl_puts(prompt);
        strncpy(last_prompt, prompt, BUF_SIZE - 1);
        change = 0;
        gl_width = gl_termw - gl_strlen(prompt);
    } else if (strcmp(prompt, last_prompt) != 0) {
        old_prompt_w = gl_strlen(last_prompt);
        strncpy(last_prompt, prompt, BUF_SIZE - 1);
        gl_width = gl_termw - gl_strlen(prompt);
        prompt_redraw = 1;
        change = 0;
    }
    if (gl_width < 8)
        gl_width = 8;
    if (change >= 0) {
        gl_cnt = strlen(gl_buf);
        redraw = 1;
    }
    if (cursor > gl_cnt) {
        if (cursor != BUF_SIZE)  /* BUF_SIZE means end of line */
            gl_putc('\007');
        cursor = gl_cnt;
    }
    if (cursor < 0) {
        gl_putc('\007');
        cursor = 0;
    }
    cursor = gl_u8_snap(cursor);
    cursor_col = gl_u8_colwidth(gl_buf, 0, cursor);

    /* choose the horizontal scroll position, in columns */
    if (gl_u8_colwidth(gl_buf, 0, gl_cnt) <= gl_width - 1) {
        new_shift_col = 0;  /* whole line fits */
    } else {
        new_shift_col = cursor_col + 2 + gl_scroll - gl_width;
        if (new_shift_col > 0) {
            new_shift_col /= gl_scroll;
            new_shift_col *= gl_scroll;
        } else
            new_shift_col = 0;
    }
    /* first character starting at or after new_shift_col */
    new_shift = 0;
    col = 0;
    while (new_shift < gl_cnt && col < new_shift_col) {
        n = gl_u8_decode(gl_buf + new_shift, gl_cnt - new_shift, &cp);
        col += gl_u8_width(cp);
        new_shift += n;
    }
    new_shift = gl_u8_snap(new_shift);
    if (new_shift > cursor)
        new_shift = cursor;  /* keep the cursor on screen */
    if (new_shift != gl_shift) {
        gl_shift = new_shift;
        shift_col = gl_u8_colwidth(gl_buf, 0, gl_shift);
        redraw = 1;
    }

    if (redraw || prompt_redraw) {
        int old_total = old_prompt_w + drawn;  /* width to overwrite */
        int full = prompt_redraw || (gl_shift > 0) != off_left ||
                   change < 0 || change <= gl_shift;

        off_left = (gl_shift > 0);
        if (prompt_redraw) {
            /* gl_putc() drops '\r', so back up over old prompt and input */
            for (i = 0; i < old_prompt_w + scr_col; i++)
                gl_putc('\b');
            gl_puts(prompt);
        }
        if (!full) {
            /* only text from `change` on differs: redraw from there */
            p = gl_u8_snap(change);
            col = gl_u8_colwidth(gl_buf, gl_shift, p) + (off_left? 1 : 0);
            if (col <= scr_col && col <= drawn) {
                for (i = col; i < scr_col; i++)
                    gl_putc('\b');
            } else {
                full = 1;  /* cursor is left of the change: start over */
            }
        }
        if (full) {
            if (!prompt_redraw)
                for (i = 0; i < scr_col; i++)  /* back to start of input */
                    gl_putc('\b');
            col = 0;
            if (off_left) {
                gl_putc('$');
                col = 1;
            }
            p = gl_shift;
        }
        /* draw characters until the right edge (leave room for '$') */
        limit = gl_width - 1;
        off_right = 0;
        while (p < gl_cnt) {
            n = gl_u8_decode(gl_buf + p, gl_cnt - p, &cp);
            w = gl_u8_width(cp);
            /* would this char leave no room for a '$' if more follows? */
            if (col + w > limit - ((p + n < gl_cnt)? 1 : 0)) {
                off_right = 1;
                break;
            }
            write_ignore(1, gl_buf + p, n);
            col += w;
            p += n;
        }
        if (off_right) {
            gl_putc('$');
            col++;
        }
        /* erase the remains of the previous line */
        if (prompt_redraw)
            w = old_total - (int)gl_strlen(prompt) - col;
        else
            w = drawn - col;
        if (w < 0)
            w = 0;
        for (i = 0; i < w; i++)
            gl_putc(' ');
        drawn = col;
        scr_col = col + w;
    }

    /* move the terminal cursor to the requested position */
    col = cursor_col - shift_col + (off_left? 1 : 0);
    if (col < scr_col) {
        for (i = col; i < scr_col; i++)
            gl_putc('\b');
    } else if (col > scr_col) {
        /* re-emit the characters between the current and new position */
        p = gl_shift;
        i = (off_left? 1 : 0);
        while (p < cursor && i < scr_col) {  /* find where we are */
            n = gl_u8_decode(gl_buf + p, gl_cnt - p, &cp);
            i += gl_u8_width(cp);
            p += n;
        }
        if (i == scr_col) {
            write_ignore(1, gl_buf + p, cursor - p);
        } else {  /* should not happen; redraw from the start */
            for (i = 0; i < scr_col; i++)
                gl_putc('\b');
            if (off_left)
                gl_putc('$');
            write_ignore(1, gl_buf + gl_shift, cursor - gl_shift);
        }
    }
    scr_col = col;
    gl_pos = cursor;
}

static int
gl_tab(buf, offset, loc)
    char *buf;
    int offset;
    int *loc;
    /* default tab handler, acts like tabstops every 8 cols */
{
    int i, count, len;

    len = strlen(buf);
    count = 8 - (offset + gl_u8_colwidth(buf, 0, *loc)) % 8;
    if (len + count >= BUF_SIZE - 1)
        return -1;
    for (i = len; i >= *loc; i--)
        buf[i+count] = buf[i];
    for (i = 0; i < count; i++)
        buf[*loc+i] = ' ';
    i = *loc;
    *loc = i + count;
    return i;
}

/******************* strlen stuff **************************************/

void gl_strwidth(func)
    size_t (*func)(const char *);
{
    if (func != 0) {
        gl_strlen = func;
    }
}

/******************* History stuff **************************************/

#ifndef HIST_SIZE
#define HIST_SIZE 100
#endif

static int hist_pos = 0, hist_last = 0;
static char *hist_buf[HIST_SIZE];

static void
hist_init()
{
    int i;

    hist_buf[0] = "";
    for (i = 1; i < HIST_SIZE; i++)
        hist_buf[i] = (char *)0;
}

void
gl_histadd(buf)
    char *buf;
{
    static char *prev = 0;
    char *p = buf;
    int len;

    /* in case we call gl_histadd() before we call bp_getline() */
    if (gl_init_done < 0) {  /* -1 only on startup */
        hist_init();
        gl_init_done = 0;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;
    if (*p) {
        len = strlen(buf);
        if (strchr(p, '\n'))  /* previously line already has NL stripped */
            len--;
        if (prev == 0 || strlen(prev) != len ||
            strncmp(prev, buf, len) != 0) {
            hist_buf[hist_last] = hist_save(buf);
            prev = hist_buf[hist_last];
            hist_last = (hist_last + 1) % HIST_SIZE;
            if (hist_buf[hist_last] && *hist_buf[hist_last]) {
                free(hist_buf[hist_last]);
            }
            hist_buf[hist_last] = "";
        }
    }
    hist_pos = hist_last;
}

static char *
hist_prev()
/* loads previous hist entry into input buffer, sticks on first */
{
    char *p = 0;
    int next = (hist_pos - 1 + HIST_SIZE) % HIST_SIZE;

    if (hist_buf[hist_pos] != 0 && next != hist_last) {
        hist_pos = next;
        p = hist_buf[hist_pos];
    }
    if (p == 0) {
        p = "";
        gl_putc('\007');
    }
    return p;
}

static char *
hist_next()
/* loads next hist entry into input buffer, clears on last */
{
    char *p = 0;

    if (hist_pos != hist_last) {
        hist_pos = (hist_pos+1) % HIST_SIZE;
        p = hist_buf[hist_pos];
    }
    if (p == 0) {
        p = "";
        gl_putc('\007');
    }
    return p;
}

static char *
hist_save(p)
    char *p;
    /* makes a copy of the string */
{
    char *s = 0;
    int len = strlen(p);
    char *nl = strchr(p, '\n');

    if (nl) {
        if ((s = (char *)malloc(len)) != 0) {
            strncpy(s, p, len-1);
            s[len-1] = 0;
        }
    } else {
        if ((s = (char *)malloc(len+1)) != 0) {
            strcpy(s, p);
        }
    }
    if (s == 0)
        gl_error("\n*** Error: hist_save() failed on malloc\n");
    return s;
}

/******************* Search stuff **************************************/

static char search_prompt[101];  /* prompt includes search string */
static char search_string[100];
static int search_pos = 0;  /* current location in search_string */
static int search_forw_flg = 0;  /* search direction flag */
static int search_last = 0;  /* last match found */

static void
search_update(c)
    int c;
{
    if (c == 0) {
        search_pos = 0;
        search_string[0] = 0;
        search_prompt[0] = '?';
        search_prompt[1] = ' ';
        search_prompt[2] = 0;
    } else if (c > 0) {
        if (search_pos >= (int)sizeof(search_string) - 1) {
            gl_putc('\007');
            return;
        }
        search_string[search_pos] = c;
        search_string[search_pos+1] = 0;
        search_prompt[search_pos] = c;
        search_prompt[search_pos+1] = '?';
        search_prompt[search_pos+2] = ' ';
        search_prompt[search_pos+3] = 0;
        search_pos++;
    } else {
        if (search_pos > 0) {
            search_pos--;  /* remove a whole UTF-8 character */
            while (search_pos > 0 &&
                   ((unsigned char)search_string[search_pos] & 0xC0) == 0x80)
                search_pos--;
            search_string[search_pos] = 0;
            search_prompt[search_pos] = '?';
            search_prompt[search_pos+1] = ' ';
            search_prompt[search_pos+2] = 0;
        } else {
            gl_putc('\007');
            hist_pos = hist_last;
        }
    }
}

static void
search_addchar(c)
    int c;
{
    char *loc;

    search_update(c);
    if (c < 0) {
        if (search_pos > 0) {
            hist_pos = search_last;
        } else {
            gl_buf[0] = 0;
            hist_pos = hist_last;
        }
        strcpy(gl_buf, hist_buf[hist_pos]);
    }
    if ((loc = strstr(gl_buf, search_string)) != 0) {
        gl_fixup(search_prompt, 0, loc - gl_buf);
    } else if (search_pos > 0) {
        if (search_forw_flg) {
            search_forw(0);
        } else {
            search_back(0);
        }
    } else {
        gl_fixup(search_prompt, 0, 0);
    }
}

static void
search_addstr(s, n)
    const char *s;
    int n;
    /* add one UTF-8 encoded character to the search string */
{
    int i;

    if (search_pos + n >= (int)sizeof(search_string) - 1) {
        gl_putc('\007');
        return;
    }
    for (i = 0; i < n - 1; i++)  /* all but the last byte, no search yet */
        search_update((unsigned char)s[i]);
    search_addchar((unsigned char)s[n-1]);
}

static void
search_term()
{
    gl_search_mode = 0;
    if (gl_buf[0] == 0)  /* not found, reset hist list */
        hist_pos = hist_last;
    if (gl_in_hook)
        gl_in_hook(gl_buf);
    gl_fixup(gl_prompt, 0, gl_pos);
}

static void
search_back(new_search)
    int new_search;
{
    int found = 0;
    char *p, *loc;

    search_forw_flg = 0;
    if (gl_search_mode == 0) {
        search_last = hist_pos = hist_last;
        search_update(0);
        gl_search_mode = 1;
        gl_buf[0] = 0;
        gl_fixup(search_prompt, 0, 0);
    } else if (search_pos > 0) {
        while (!found) {
            p = hist_prev();
            if (*p == 0) {  /* not found, done looking */
                gl_buf[0] = 0;
                gl_fixup(search_prompt, 0, 0);
                found = 1;
            } else if ((loc = strstr(p, search_string)) != 0) {
                strcpy(gl_buf, p);
                gl_fixup(search_prompt, 0, loc - p);
                if (new_search)
                    search_last = hist_pos;
                found = 1;
            }
        }
    } else {
        gl_putc('\007');
    }
}

static void
search_forw(new_search)
    int new_search;
{
    int found = 0;
    char *p, *loc;

    search_forw_flg = 1;
    if (gl_search_mode == 0) {
        search_last = hist_pos = hist_last;
        search_update(0);
        gl_search_mode = 1;
        gl_buf[0] = 0;
        gl_fixup(search_prompt, 0, 0);
    } else if (search_pos > 0) {
        while (!found) {
            p = hist_next();
            if (*p == 0) {  /* not found, done looking */
                gl_buf[0] = 0;
                gl_fixup(search_prompt, 0, 0);
                found = 1;
            } else if ((loc = strstr(p, search_string)) != 0) {
                strcpy(gl_buf, p);
                gl_fixup(search_prompt, 0, loc - p);
                if (new_search)
                    search_last = hist_pos;
                found = 1;
            }
        }
    } else {
        gl_putc('\007');
    }
}

char *nogl_getline(prompt)
    char *prompt;
{
    int c;  /* int, not char: bytes >= 0x80 must not look like EOF */
    int i = 0;
    printf("%s", prompt);
    fflush(curr_out);
    while ((c = getc(curr_in)) != EOF) {
        if (i >= BUF_SIZE - 1) quit("\n*** Error: nogl_getline input buffer overflow\n");
        gl_buf[i++] = (char)c;
        if (c == '\n' || c == '\r') {
            gl_buf[i] = '\0';
            return gl_buf;
        }
    }
    exit(0);
}

int c_GET_NONEMPTY_LINE() {
#ifdef PICAT
    char *prompt = "Picat> ";
#else
    char *prompt = "| ?- ";
#endif
    c_init_chars_pool();
    string_in = NULL;
    while (string_in == NULL || getline_is_spaces(string_in)) {
        if (use_gl_getline)
            string_in = bp_getline(prompt);
        else
            string_in = nogl_getline(prompt);
    }
    if (use_gl_getline) gl_histadd(string_in);
    lastc = ' ';
    printf("\n");
    return BP_TRUE;
}

int getline_is_spaces(s)
    char *s;
{
    if (*s == '\n' || *s == '\r' || (unsigned char)*s <= 31) {
        printf("\n");
        return 1;
    }

    while (*s != '\0') {
        if (*s == ' ' || *s == '\t')
            s++;
        else if (*s == '\n' || *s == '\r') {
            printf("\n"); s++;
        } else if (*s == '%') {
            printf("\n");
            return 1;
        } else return 0;
    }
    return 1;
}

int c_FINISH_GET_LINE() {
    string_in = NULL;
    lastc = ' ';
    /*
      #ifdef WIN32
      #ifndef NT
      getc(stdin);
      #endif
      #endif
    */
    return 1;
}

/******************* UTF-8 helpers **************************************/

static int
gl_u8_seqlen(c)
    int c;
/* length of the UTF-8 sequence that starts with byte c, 0 if invalid */
{
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 0;
}

static int
gl_u8_decode(s, max, cp)
    const char *s;
    int max;
    int *cp;
/*
 * Decode the character at s (at most max bytes available).  Stores the
 * code point in *cp and returns the number of bytes used.  Malformed input
 * yields U+FFFD and consumes one byte, so callers always make progress.
 */
{
    const unsigned char *u = (const unsigned char *)s;
    int n, i, c;

    if (max <= 0) {
        *cp = 0;
        return 0;
    }
    n = gl_u8_seqlen(u[0]);
    if (n == 1) {
        *cp = u[0];
        return 1;
    }
    if (n == 0 || n > max)
        goto bad;
    c = u[0] & (0x7F >> n);
    for (i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80)
            goto bad;
        c = (c << 6) | (u[i] & 0x3F);
    }
    /* reject overlong forms, surrogates and values past U+10FFFF */
    if ((n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
        (c >= 0xD800 && c <= 0xDFFF))
        goto bad;
    *cp = c;
    return n;
bad:
    *cp = 0xFFFD;
    return 1;
}

static int
gl_u8_width(c)
    int c;
/* number of terminal columns used by code point c (0, 1 or 2) */
{
    if (c == 0)
        return 0;
    if (c < 0x20 || (c >= 0x7F && c < 0xA0))
        return 1;  /* control chars: assume they take a column */
    /* zero width: combining marks, ZWSP/ZWJ/ZWNJ, variation selectors */
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) ||
        (c >= 0x0591 && c <= 0x05BD) || (c >= 0x0610 && c <= 0x061A) ||
        (c >= 0x064B && c <= 0x065F) || (c >= 0x0E31 && c <= 0x0E3A && c != 0x0E32 && c != 0x0E33) ||
        (c >= 0x0E47 && c <= 0x0E4E) ||
        (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
        (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) ||
        (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
        c == 0xFEFF || (c >= 0xE0100 && c <= 0xE01EF))
        return 0;
    /* double width: East Asian wide and fullwidth forms, emoji */
    if ((c >= 0x1100 && c <= 0x115F) || c == 0x2329 || c == 0x232A ||
        (c >= 0x231A && c <= 0x231B) || (c >= 0x23E9 && c <= 0x23EC) ||
        (c >= 0x25FD && c <= 0x25FE) || (c >= 0x2614 && c <= 0x2615) ||
        (c >= 0x2648 && c <= 0x2653) || (c >= 0x26AA && c <= 0x26AB) ||
        (c >= 0x26BD && c <= 0x26BE) || (c >= 0x26C4 && c <= 0x26C5) ||
        c == 0x26A1 || c == 0x26CE || c == 0x26D4 || c == 0x26EA ||
        (c >= 0x26F2 && c <= 0x26F5) || c == 0x26FA || c == 0x26FD ||
        c == 0x2705 || (c >= 0x270A && c <= 0x270B) || c == 0x2728 ||
        c == 0x274C || c == 0x274E || (c >= 0x2753 && c <= 0x2755) ||
        c == 0x2757 || (c >= 0x2795 && c <= 0x2797) || c == 0x27B0 ||
        c == 0x27BF || (c >= 0x2B1B && c <= 0x2B1C) || c == 0x2B50 ||
        c == 0x2B55 ||
        (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
        (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xA960 && c <= 0xA97F) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE10 && c <= 0xFE19) || (c >= 0xFE30 && c <= 0xFE6F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) ||
        (c >= 0x16FE0 && c <= 0x18CFF) || (c >= 0x1B000 && c <= 0x1B2FF) ||
        (c >= 0x1F004 && c <= 0x1F004) || c == 0x1F0CF || c == 0x1F18E ||
        (c >= 0x1F191 && c <= 0x1F19A) || (c >= 0x1F200 && c <= 0x1F251) ||
        (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F680 && c <= 0x1F6FF) ||
        (c >= 0x1F7E0 && c <= 0x1F7EB) || (c >= 0x1F90C && c <= 0x1F9FF) ||
        (c >= 0x1FA70 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x2FFFD) || (c >= 0x30000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static int
gl_u8_colwidth(s, from, to)
    const char *s;
    int from, to;
/* display width of the bytes s[from..to) */
{
    int w = 0, n, cp;

    while (from < to) {
        n = gl_u8_decode(s + from, to - from, &cp);
        w += gl_u8_width(cp);
        from += n;
    }
    return w;
}

static size_t
gl_u8_strwidth(s)
    const char *s;
/* display width of a NUL-terminated UTF-8 string (default gl_strlen) */
{
    return (size_t)gl_u8_colwidth(s, 0, (int)strlen(s));
}

static int
gl_u8_snap(pos)
    int pos;
/* round a byte offset in gl_buf down to the start of a character */
{
    while (pos > 0 && ((unsigned char)gl_buf[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static int
gl_u8_next(pos)
    int pos;
/*
 * Offset just past the character at pos, including any zero-width
 * combining marks that follow it, so the cursor never lands between a
 * base character and its accents.
 */
{
    int n, cp;
    int len = (int)strlen(gl_buf);

    if (pos >= len)
        return len;
    pos += gl_u8_decode(gl_buf + pos, len - pos, &cp);
    while (pos < len) {
        n = gl_u8_decode(gl_buf + pos, len - pos, &cp);
        if (gl_u8_width(cp) != 0)
            break;
        pos += n;
    }
    return pos;
}

static int
gl_u8_prev(pos)
    int pos;
/* offset of the start of the character before pos (see gl_u8_next) */
{
    int p, cp;

    while (pos > 0) {
        p = gl_u8_snap(pos - 1);
        gl_u8_decode(gl_buf + p, pos - p, &cp);
        pos = p;
        if (gl_u8_width(cp) != 0)
            break;  /* stop on a base character */
    }
    return pos;
}

