/*
 * Copyright (c) 2000 Satoru Takabayashi <satoru@namazu.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#define _GNU_SOURCE /* need sighandler_t */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#ifdef HAVE_KQUEUE
#include <sys/event.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <curses.h>
#include <signal.h>
#include <errno.h>

#include "dgamelaunch.h"
#include "ttyplay.h"
#include "ttyrec.h"
#include "io.h"
#include "stripgfx.h"

int stripped = NO_GRAPHICS;
static int got_sigwinch = 0;

static int term_resizex = -1;
static int term_resizey = -1;

void
ttyplay_sigwinch_func(int sig)
{
    signal(SIGWINCH, ttyplay_sigwinch_func);
    got_sigwinch = 1;
}

struct timeval
timeval_diff (struct timeval tv1, struct timeval tv2)
{
  struct timeval diff;

  diff.tv_sec = tv2.tv_sec - tv1.tv_sec;
  diff.tv_usec = tv2.tv_usec - tv1.tv_usec;
  if (diff.tv_usec < 0)
    {
      diff.tv_sec--;
      diff.tv_usec += 1000000;
    }

  return diff;
}

struct timeval
timeval_div (struct timeval tv1, double n)
{
  double x = ((double) tv1.tv_sec + (double) tv1.tv_usec / 1000000.0) / n;
  struct timeval div;

  div.tv_sec = (int) x;
  div.tv_usec = (x - (int) x) * 1000000;

  return div;
}

double
ttywait (struct timeval prev, struct timeval cur, double speed)
{
  struct timeval diff = timeval_diff (prev, cur);

  assert (speed != 0);
  diff = timeval_div (diff, speed);

  select (1, NULL, NULL, NULL, &diff); /* skip if a user hits any key */
  
  return speed;
}

double
ttynowait (struct timeval prev, struct timeval cur, double speed)
{
  return 0;                     /* Speed isn't important. */
}

int
ttyread (FILE * fp, Header * h, char **buf, int pread)
{
  long offset;

  /* do this BEFORE header read, hlen bug */
  offset = ftell (fp);

  if (read_header (fp, h) == 0)
    {
      return READ_EOF;
    }

  /* length should never be longer than one BUFSIZ */
  if (h->len > BUFSIZ)
    {
      fprintf (stderr, "h->len too big (%ld) limit %ld\n",
		      (long)h->len, (long)BUFSIZ);
      exit (-21);
    }

  *buf = malloc (h->len + 1);
  if (*buf == NULL)
    {
      perror ("malloc");
      exit (-22);
    }

  if (fread (*buf, 1, h->len, fp) != h->len)
    {
      fseek (fp, offset, SEEK_SET);
      return READ_EOF;
    }
  (*buf)[h->len] = 0;
  return READ_DATA;
}

int
ttypread (FILE * fp, Header * h, char **buf, int pread)
{
  int n;
#ifdef HAVE_KQUEUE
  struct kevent evt[2];
  static int kq = -1;
#endif
  struct timeval w = { 0, 100000 };
  struct timeval origw = { 0, 100000 };
  int counter = 0;
  fd_set readfs;
  struct termios t;
  int doread = 0;
  static int tried_resize = 0;

#ifdef HAVE_KQUEUE
  if (kq == -1)
    kq = kqueue ();
  if (kq == -1)
    {
      printf ("kqueue() failed.\n");
      exit (1);
    }
#endif

  /*
   * Read persistently just like tail -f.
   */
  while (ttyread (fp, h, buf, 1) == READ_EOF)
    {
      fflush(stdout);
      clearerr (fp);
#ifdef HAVE_KQUEUE
      n = -1;
      if (kq != -2)
      {
	EV_SET (&evt[0], STDIN_FILENO, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
	EV_SET (&evt[1], fileno (fp), EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
	n = kevent (kq, evt, 2, evt, 1, NULL);
	doread = (n >= 1 && evt[0].ident == STDIN_FILENO &&
	  evt[0].filter == EVFILT_READ) ||
	  (n >= 2 && evt[1].ident == STDIN_FILENO &&
	   evt[1].filter == EVFILT_READ);
	if (n == -1)
	  {
	    /*
	     * Perhaps kevent(2) doesn't work on this fstype,
	     * use select(2) instead. Never use kevent again, assuming all
	     * active ttyrecs are on the same fstype.
	     */
	    close(kq);
	    kq = -2;
	  }
      }
      if (n == -1)
#endif
      {
	if (counter++ > (20 * 60 * 10))
	  {
	    /*
	     * The reason for this timeout is that the select() method uses
	     * some CPU in waiting. The kqueue() method does not do that, so it
	     * does not need the timeout.
	     */
	    endwin ();
	    printf ("Exiting due to 20 minutes of inactivity.\n");
	    exit (-23);
	  }
	FD_ZERO (&readfs);
	FD_SET (STDIN_FILENO, &readfs);
	n = select (1, &readfs, NULL, NULL, &w);
	w = origw;
	doread = n >= 1 && FD_ISSET (0, &readfs);
      }
      if (n == -1)
	{
	    if ((errno == EINTR) && got_sigwinch) {
		got_sigwinch = 0;
		return READ_RESTART;
	    } else {
		printf("select()/kevent() failed.\n");
		exit (1);
	    }
	}
      if (doread)
        {                       /* user hits a character? */
          char c;
          read (STDIN_FILENO, &c, 1); /* drain the character */

          switch (c)
            {
            case 'q':
              return READ_EOF;
              break;
	    case 'r':
		if (term_resizex > 0 && term_resizey > 0) {
		    printf ("\033[8;%d;%dt", term_resizey, term_resizex);
		    return READ_RESTART;
		}
		break;
	    case 's':
	      switch (stripped)
	      {
		case NO_GRAPHICS: populate_gfx_array ((stripped = DEC_GRAPHICS)); break;
		case DEC_GRAPHICS: populate_gfx_array ((stripped = IBM_GRAPHICS)); break;
		case IBM_GRAPHICS: populate_gfx_array ((stripped = NO_GRAPHICS)); break;
	      }
	      return READ_RESTART;
	      break;

            case 'm':
	      tcgetattr (0, &t);
	      if (!loggedin)
	      {
		initcurses();
		loginprompt(1);
	      }
              if (loggedin)
	      {
		initcurses ();
		domailuser (chosen_name);
	      }
              endwin ();
	      tcsetattr (0, TCSANOW, &t);
              return READ_RESTART;
              break;
            case '?':
		tcgetattr (0, &t);
		initcurses();
		(void) runmenuloop(dgl_find_menu("watchmenu_help"));
		endwin ();
		tcsetattr (0, TCSANOW, &t);
		return READ_RESTART;
		break;
            }
        }
    }
  return READ_DATA;
}

void
ttywrite (char *buf, int len)
{
  int i;

  for (i = 0; i < len; i++)
  {
    if (stripped != NO_GRAPHICS)
      buf[i] = strip_gfx (buf[i]);
  }

  fwrite (buf, 1, len, stdout);
}

void
ttynowrite (char *buf, int len)
{
  /* do nothing */
}

int
ttyplay (FILE * fp, double speed, ReadFunc read_func,
         WriteFunc write_func, WaitFunc wait_func, off_t offset)
{
  int first_time = 1;
  int r = READ_EOF;
  struct timeval prev;

  /* for dtype's attempt to get the last clrscr and playback from there */
  if (offset != -1)
    {
      fseek (fp, offset, SEEK_SET);
    }

  while (1)
    {
      char *buf;
      Header h;

      r = read_func (fp, &h, &buf, 0);
      if (r != READ_DATA)
        {
          break;
        }

      if (!first_time)
        {
          speed = wait_func (prev, h.tv, speed);
        }
      first_time = 0;

      write_func (buf, h.len);
      prev = h.tv;
      free (buf);
    }
  return r;
}

static off_t
find_seek_offset_clrscr (FILE * fp)
{
  off_t raw_seek_offset = 0;
  off_t seek_offset_clrscr;
  char *buf;
  struct stat mystat;
  int state = 0;
  int i;
  int bytesread;

  fstat (fileno (fp), &mystat);
  buf = malloc (mystat.st_size);
  fseek (fp, 0, SEEK_SET);
  bytesread = fread (buf, 1, mystat.st_size, fp);

  /* one byte at at time sucks, but is a simple hack for the temp
   * being to avoid looking for wraparounds */
  for (i = 0; i < bytesread; i++)
    {
      if (buf[i] == 0x1b)
        {
          state = 1;
        }
      else if ((buf[i] == 0x5b) && (state == 1))
        {
          state = 2;
        }
      else if ((buf[i] == 0x32) && (state == 2))
        {
          state = 3;
        }
      else if ((buf[i] == 0x4a) && ((state == 2) || (state == 3)))
        {
          state = 4;
        }
      else
        {
          state = 0;
        }

      if (state == 4)
        {
          raw_seek_offset = i - 2;
        }
    }

  free (buf);

  seek_offset_clrscr = 0;
  /* now find last filepos that is less than seek offset */
  fseek (fp, 0, SEEK_SET);
  while (1)
    {
      char *buf;
      Header h;

      if (ttyread (fp, &h, &buf, 0) != READ_DATA)
        {
          break;
        }

      if (ftell (fp) < raw_seek_offset)
        {
          seek_offset_clrscr = ftell (fp);
        }

      free (buf);
    }

  return seek_offset_clrscr;
}

#if 0 /* not used anymore */
void
ttyskipall (FILE * fp)
{
  /*
   * Skip all records.
   */
  ttyplay (fp, 0, ttyread, ttynowrite, ttynowait, 0);
}
#endif

void
ttyplayback (FILE * fp, double speed, ReadFunc read_func, WaitFunc wait_func)
{
  ttyplay (fp, speed, ttyread, ttywrite, wait_func, 0);
}

void
ttypeek (FILE * fp, double speed)
{
  int r;

  do
  {
    setvbuf (fp, NULL, _IOFBF, 0);
    ttyplay (fp, 0, ttyread, ttywrite, ttynowait, find_seek_offset_clrscr (fp));
    clearerr (fp);
    setvbuf (fp, NULL, _IONBF, 0);
    fflush (stdout);
    r = ttyplay (fp, speed, ttypread, ttywrite, ttynowait, -1);
  } while (r == READ_RESTART);
}


int
ttyplay_main (char *ttyfile, int mode, int resizex, int resizey)
{
  double speed = 1.0;
  ReadFunc read_func = ttyread;
  WaitFunc wait_func = ttywait;
  FILE *input = stdin;
  struct termios old, new;
  sighandler_t old_sigwinch;

  populate_gfx_array (stripped);

  input = efopen (ttyfile, "r");

  tcgetattr (0, &old);          /* Get current terminal state */
  new = old;                    /* Make a copy */
  new.c_lflag &= ~(ICANON | ECHO | ECHONL); /* unbuffered, no echo */
  new.c_cc[VMIN] = 1;
  new.c_cc[VTIME] = 0;
  tcsetattr (0, TCSANOW, &new); /* Make it current */


  if (resizex > 0 && resizey > 0) {
      term_resizex = resizex;
      term_resizey = resizey;
  }

  got_sigwinch = 0;
  old_sigwinch = signal(SIGWINCH, ttyplay_sigwinch_func);

  if (mode == 1)
    ttypeek (input, speed);
  else
    ttyplayback (input, speed, read_func, wait_func);

  tcsetattr (0, TCSANOW, &old); /* Return terminal state */
  fclose (input);

  if (old_sigwinch != SIG_ERR)
      signal(SIGWINCH, old_sigwinch);

  term_resizex = term_resizey = -1;

  return 0;
}
