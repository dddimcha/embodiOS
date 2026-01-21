/* Kernel stub for signal.h
 * For llama.cpp bare-metal compatibility
 * No actual signal handling in bare-metal - just stubs
 */

#ifndef _COMPAT_SIGNAL_H
#define _COMPAT_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Signal numbers */
#define SIGABRT  6
#define SIGFPE   8
#define SIGILL   4
#define SIGINT   2
#define SIGSEGV  11
#define SIGTERM  15

/* Signal handler type */
typedef void (*sighandler_t)(int);

/* Special signal handlers */
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* Signal set type */
typedef unsigned long sigset_t;

/* Signal action structure */
struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

/* Signal functions - stubs */
sighandler_t signal(int signum, sighandler_t handler);
int raise(int sig);
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int signum);
int sigdelset(sigset_t* set, int signum);
int sigismember(const sigset_t* set, int signum);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_SIGNAL_H */
