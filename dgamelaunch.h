/* IMPORTANT defines */

#ifndef __DGAMELAUNCH_H
#define __DGAMELAUNCH_H

#include <sys/types.h>
#include <time.h>

/* Default - should work everywhere */
#define USE_OPENPTY
#define NOSTREAMS

struct dg_user
{
  char *username;
  char *email;
  char *env;
  char *password;
  int flags;
};

struct dg_banner
{
  char **lines;
  unsigned int len;
};

struct dg_game
{
  char *ttyrec_fn;
  char *name;
  char *date;
  char *time;
  time_t idle_time;
};

struct dg_config
{
  char* chroot;
  char* nethack;
  char* dglroot;
  char* banner;
  char* rcfile;
  char* spool;
  char* shed_user;
  char* shed_group;
  uid_t shed_uid;
  gid_t shed_gid;
  unsigned long max;
};

extern char* config; /* file path */
extern struct dg_config *myconfig;
extern struct dg_config defconfig;

/* dgamelaunch.c function prototypes */
extern void create_config (void);
extern void ttyrec_getmaster (void);
extern void gen_ttyrec_filename (void);
extern void gen_inprogress_lock (pid_t pid);
extern void catch_sighup (int signum);
extern void loadbanner (struct dg_banner *ban);
extern void drawbanner (unsigned int start_line, unsigned int howmany);
extern struct dg_game **populate_games (int *l);
extern void inprogressmenu (void);
extern int changepw (void);
extern void domailuser (char *username);
extern void drawmenu (void);
extern void freefile (void);
extern void initncurses (void);
extern struct dg_user *deep_copy (struct dg_user *src);
extern void loginprompt (void);
extern void newuser (void);
extern int passwordgood (char *cpw);
extern int readfile (int nolock);
extern int userexist (char *cname);
extern void write_canned_rcfile (char *target);
extern void editoptions (void);
extern void writefile (int requirenew);
extern void graceful_exit (int status);

/* strlcpy.c */
extern size_t strlcpy (char *dst, const char *src, size_t siz);
extern size_t strlcat (char *dst, const char *src, size_t siz);

/* mygetnstr.c */
extern int mygetnstr(char *buf, int maxlen, int doecho);

#endif
