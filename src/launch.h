/*
 *  Window Maker window manager
 *
 *  Launching programs (DAR-434, anurodhp/wmaker iokit-port fork).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef WMLAUNCH_H_
#define WMLAUNCH_H_

#include <sys/types.h>
#include "screen.h"

/* QoS of the child: WSpawnQoSDefault leaves it alone (what fork+exec gave),
 * WSpawnQoSUtility clamps it to QOS_CLASS_UTILITY (helpers). */
#define WSpawnQoSDefault 0
#define WSpawnQoSUtility 1

/*
 * Run `file` (searched in $PATH like execvp) with argv, in a new session,
 * with SetupEnvironment()'s variables set for `scr`.
 *
 * Only stdin/stdout/stderr and the descriptors named below reach the child:
 *   stdin_fd >= 0   becomes the child's stdin (dup2), the parent keeps its copy
 *   close_fd >= 0   is closed in the child (only matters for the fork
 *                   fallback; the spawn path closes everything else anyway)
 *
 * Returns the pid, or -1 with errno set. An exec failure (no such program)
 * is reported here by the posix_spawn path (ENOENT, EACCES, ...); the fork
 * fallback can only report it as the child's exit status 111.
 *
 * With the kqueue event loop the exit of the child is watched with
 * EVFILT_PROC and delivered to the death handlers from the main loop;
 * the SIGCHLD handler (startup.c) stays as the fallback.
 */
pid_t wSpawn(WScreen *scr, const char *file, char *const argv[], int stdin_fd, int close_fd, int qos);

/* Make the main loop run DispatchEvent() soon (death handlers are run there):
 * sends this process a ClientMessage on scr's info window. */
void wSpawnWake(WScreen *scr);

/* Called with every pid the SIGCHLD path or the kqueue reported dead:
 * drops the pid's EVFILT_PROC watch, if it still has one. */
void wSpawnForget(pid_t pid);

#endif
