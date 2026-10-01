/*
 *  Window Maker window manager
 *
 *  Launching programs (DAR-434, anurodhp/wmaker iokit-port fork).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 * Why not fork+exec: wmaker is a large X client, and fork() on Darwin
 * copies its whole address space map (and every descriptor) for the few
 * microseconds before exec throws it away. posix_spawn() makes the new
 * process directly from the program image: no copy of the parent, and
 * POSIX_SPAWN_CLOEXEC_DEFAULT (xnu bsd/sys/spawn.h:73, honoured in
 * bsd/kern/kern_exec.c:2533-2575) closes every descriptor the parent
 * holds (the X connection, the kqueue, vnode watches, helper pipes) except
 * the ones named in the file actions. POSIX_SPAWN_SETSID (spawn.h:65,
 * kern_exec.c:3749) is the old setsid() in the child.
 *
 * Reaping: the SIGCHLD handler (startup.c buryChild) reaps with
 * waitpid(-1) from signal context. With the kqueue backend each spawned
 * pid also gets an EVFILT_PROC knote, NOTE_EXIT|NOTE_EXITSTATUS (xnu
 * bsd/sys/event.h; data is the wait status, kern_event.c filt_procevent),
 * so the death handlers run from the main loop (a zero-delay timer, so
 * death handlers that open dialogs do not run inside the kevent batch).
 * xnu raises NOTE_EXIT before the process turns into a zombie
 * (kern_exit.c:1579 proc_knote, 1642 p_stat = SZOMB, 1648 psignal SIGCHLD),
 * so the waitpid(pid, WNOHANG) here often finds nothing yet; the SIGCHLD
 * handler that follows reaps it. Either way the handlers are told once:
 * handleDeadProcess() removes a handler when it fires, so a second
 * NotifyDeadProcess for the same pid finds none.
 *
 * WM_SPAWN=fork selects the old fork+exec path at run time, without
 * WM_USE_POSIX_SPAWN it is the only one. WM_SPAWN_TRACE=1 logs each launch
 * and death to stderr.
 */

#include "wconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <mach/mach_time.h>

#include <WINGs/WUtil.h>

#include "WindowMaker.h"
#include "screen.h"
#include "window.h"
#include <WINGs/WINGs.h>
#include "main.h"
#include "event.h"
#include "launch.h"

#ifdef WM_USE_POSIX_SPAWN
# include <spawn.h>
# include <crt_externs.h>
# include <pthread/qos.h>
# include <pthread/spawn.h>
# ifdef WM_USE_KQUEUE
#  include <sys/event.h>
#  include <WINGs/WINGsP.h>
# endif
#endif

static int traceOn = -1;
static unsigned long nSpawned, nForked, nKqDeaths, nKqReaped, nFallbackDeaths;

/* ms on mach_absolute_time, the clock a child can read for itself (spawn_test --child stamp) */
static double nowMs(void)
{
	static mach_timebase_info_data_t tb;

	if (!tb.denom)
		mach_timebase_info(&tb);
	return (double) mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static int tracing(void)
{
	if (traceOn < 0)
		traceOn = getenv("WM_SPAWN_TRACE") != NULL;
	return traceOn;
}

#define TRACE(...) do { if (tracing()) { \
	fprintf(stderr, "wmaker spawn: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

#ifdef WM_USE_POSIX_SPAWN

static int useSpawn = -1;

/* The environment the child gets: environ plus what SetupEnvironment() puts
 * there for a forked child (DISPLAY for multihead, WRASTER_COLOR_RESOLUTION). */
static char **buildEnvironment(WScreen *scr)
{
	char **env = *_NSGetEnviron();
	char *add[2];
	char **out;
	int nadd = 0, n = 0, i, j;

	nadd = WSetupEnvironmentStrings(scr, add);
	while (env && env[n])
		n++;
	out = malloc(sizeof(char *) * (n + nadd + 1));
	if (!out) {
		for (i = 0; i < nadd; i++)
			free(add[i]);
		return NULL;
	}
	for (i = j = 0; i < n; i++) {
		int k, drop = 0;

		for (k = 0; k < nadd && !drop; k++) {
			size_t l = strchr(add[k], '=') - add[k] + 1;
			drop = strncmp(env[i], add[k], l) == 0;
		}
		if (!drop)
			out[j++] = env[i];
	}
	for (i = 0; i < nadd; i++)
		out[j++] = add[i];
	out[j] = NULL;
	return out;
}

static void freeEnvironment(char **out)
{
	char **env = *_NSGetEnviron();
	char **p;
	int k;

	if (!out)
		return;
	/* the strings that are not environ's own were allocated for this call */
	for (p = out; *p; p++) {
		int own = 0;

		for (k = 0; env && env[k]; k++)
			if (env[k] == *p)
				own = 1;
		if (!own)
			free(*p);
	}
	free(out);
}

# ifdef WM_USE_KQUEUE
typedef struct Watch {
	struct Watch *next;
	pid_t pid;
	W_KQueueID id;
} Watch;

static Watch *watches;
static WMHandlerID dispatchTimer;

static void removeWatch(pid_t pid, int deleteFilter)
{
	Watch **pp, *w;

	for (pp = &watches; (w = *pp) != NULL; pp = &w->next) {
		if (w->pid == pid) {
			*pp = w->next;
			if (deleteFilter)
				W_KQueueDeleteFilter(w->id);
			free(w);
			return;
		}
	}
}

static void dispatchDeaths(void *data)
{
	(void) data;
	dispatchTimer = NULL;
	wDispatchDeadProcesses();
}

static void procExit(const struct kevent *ev, void *clientData)
{
	pid_t pid = (pid_t) ev->ident;
	int wstatus = (int) ev->data;	/* NOTE_EXITSTATUS: the wait status */
	int st = 0;
	sigset_t sigs, old;
	pid_t r;

	(void) clientData;
	removeWatch(pid, 0);	/* EV_ONESHOT: the filter is gone from the list */

	/* really reap, if the kernel has finished the exit; if not, SIGCHLD follows */
	r = waitpid(pid, &st, WNOHANG);
	if (r == pid) {
		wstatus = st;
		nKqReaped++;
	}
	nKqDeaths++;
	TRACE("pid %d exited by kqueue, status 0x%x, %s", (int) pid, wstatus,
	      r == pid ? "reaped" : "left to SIGCHLD");

	/* NotifyDeadProcess() is written for the signal handler: keep it away */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGCHLD);
	sigprocmask(SIG_BLOCK, &sigs, &old);
	NotifyDeadProcess(pid, WEXITSTATUS(wstatus));
	sigprocmask(SIG_SETMASK, &old, NULL);

	if (!dispatchTimer)
		dispatchTimer = WMAddTimerHandler(0, dispatchDeaths, NULL);
}

static void watchExit(pid_t pid)
{
	struct kevent kev;
	Watch *w;

	EV_SET(&kev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXITSTATUS, 0, NULL);
	w = malloc(sizeof(Watch));
	if (!w)
		return;
	w->pid = pid;
	w->id = W_KQueueAddFilter(&kev, procExit, NULL);
	if (!w->id) {
		/* select backend, or the process is already gone (ESRCH): SIGCHLD covers it */
		free(w);
		return;
	}
	w->next = watches;
	watches = w;
}
# else
static void watchExit(pid_t pid) { (void) pid; }
# endif /* WM_USE_KQUEUE */

static pid_t spawnProgram(WScreen *scr, const char *file, char *const argv[], int stdin_fd, int qos)
{
	posix_spawnattr_t attr;
	posix_spawn_file_actions_t fa;
	char **envp;
	pid_t pid = -1;
	int fd, err;

	envp = buildEnvironment(scr);
	if (!envp) {
		errno = ENOMEM;
		return -1;
	}
	err = posix_spawnattr_init(&attr);
	if (err) {
		freeEnvironment(envp);
		errno = err;
		return -1;
	}
	err = posix_spawn_file_actions_init(&fa);
	if (err) {
		posix_spawnattr_destroy(&attr);
		freeEnvironment(envp);
		errno = err;
		return -1;
	}

	err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
	/* CLOEXEC_DEFAULT closes everything not named: keep stdio (if open) */
	for (fd = 0; fd < 3 && !err; fd++) {
		if (fd == STDIN_FILENO && stdin_fd >= 0)
			err = posix_spawn_file_actions_adddup2(&fa, stdin_fd, STDIN_FILENO);
		else if (fcntl(fd, F_GETFD) >= 0)
			err = posix_spawn_file_actions_addinherit_np(&fa, fd);
	}
	if (!err && qos == WSpawnQoSUtility)
		err = posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_UTILITY);
	if (!err)
		err = posix_spawnp(&pid, file, &fa, &attr, argv, envp);

	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	freeEnvironment(envp);
	if (err) {
		errno = err;
		return -1;
	}
	return pid;
}
#endif /* WM_USE_POSIX_SPAWN */

static pid_t forkProgram(WScreen *scr, const char *file, char *const argv[], int stdin_fd, int close_fd)
{
	pid_t pid = fork();

	if (pid == 0) {
		SetupEnvironment(scr);
#ifdef HAVE_SETSID
		setsid();
#endif
		if (close_fd >= 0)
			close(close_fd);
		if (stdin_fd >= 0) {
			close(STDIN_FILENO);
			if (dup2(stdin_fd, STDIN_FILENO) < 0)
				_exit(1);
			close(stdin_fd);
		}
		execvp(file, argv);
		_exit(111);
	}
	return pid;
}

pid_t wSpawn(WScreen *scr, const char *file, char *const argv[], int stdin_fd, int close_fd, int qos)
{
	pid_t pid;
	double t0 = tracing() ? nowMs() : 0;

#ifdef WM_USE_POSIX_SPAWN
	if (useSpawn < 0) {
		const char *e = getenv("WM_SPAWN");
		useSpawn = !(e && strcmp(e, "fork") == 0);
	}
	if (useSpawn) {
		pid = spawnProgram(scr, file, argv, stdin_fd, qos);
		if (pid > 0) {
			nSpawned++;
			watchExit(pid);
		}
		TRACE("posix_spawnp %s %s: pid %d (%s) [spawned %lu] t0=%.3f call=%.3f ms", file, argv[1] ? (argv[2] ? argv[2] : argv[1]) : "", (int) pid,
		      pid < 0 ? strerror(errno) : "ok", nSpawned, t0, tracing() ? nowMs() - t0 : 0);
		return pid;
	}
#endif
	(void) qos;
	pid = forkProgram(scr, file, argv, stdin_fd, close_fd);
	if (pid > 0)
		nForked++;
	TRACE("fork+exec %s: pid %d [forked %lu] t0=%.3f call=%.3f ms", file, (int) pid, nForked,
	      t0, tracing() ? nowMs() - t0 : 0);
	return pid;
}

void wSpawnForget(pid_t pid)
{
#if defined(WM_USE_POSIX_SPAWN) && defined(WM_USE_KQUEUE)
	Watch *w;

	for (w = watches; w; w = w->next) {
		if (w->pid == pid) {
			/* SIGCHLD got there first (or the exit event was missed) */
			nFallbackDeaths++;
			TRACE("pid %d reported by SIGCHLD, dropping its EVFILT_PROC watch [sigchld deaths %lu]",
			      (int) pid, nFallbackDeaths);
			removeWatch(pid, 1);
			return;
		}
	}
#else
	(void) pid;
#endif
}
