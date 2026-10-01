# Window Maker on Darwin/XNU for the Raspberry Pi 3: port notes and performance work

This fork (`iokit-port` branch, off `wmaker-0.96.0`) is the Window Maker used by the
[xnu-iokit-pi3](https://github.com/anurodhp/xnu-iokit-pi3) project: a from-source port of XNU
(Darwin 20.3 / xnu-7195) to the Raspberry Pi 3 (BCM2837). It runs on Xorg (the
`puredarwingop` driver on IOBootFramebuffer, USB input through IOHID) and is started by xdm
under launchd. It is built with Xcode 12's iPhoneOS SDK (`arm64-apple-ios14.4`) by
`tools/userland_staging/build_wmaker.sh` in that repo. The kernel currently runs on a single
core (see DAR-455 in that project's tracker).

Upstream's README follows in `README`. This file only covers what differs here.

## Performance changes

The tracker label is `windowmakermach`. Status as of 2026-09-30. Numbers marked **QEMU** are
from QEMU's TCG emulation, which is only an indication (run-to-run noise was up to 2x between
boots); numbers for the real Pi are noted as pending where they have not been taken.

| Change | Where | Status | Effect |
|---|---|---|---|
| `ProcessType=Interactive` for the X session job | xnu-iokit-pi3: `x11_config/session/org.puredarwin.x11-session.plist` (DAR-439 step 1) | Done, confirmed on the Pi | The UI became much faster (user-observed on the real Pi; not benchmarked). See below. |
| Build the whole X11 stack, wrlib, WINGs and wmaker at `-O2` | xnu-iokit-pi3: `x11_common.sh` (`X11_OPT`, default `-O2`) (DAR-438) | Done on QEMU, merged | About 1.3x to 1.8x on wrlib and Xlib micro-benchmarks (table below). |
| wrlib 32bpp TrueColor fast path and NEON blending | this fork, commit `1ef1522c` (DAR-437) | Done on QEMU, merged | `RConvertImage + XCopyArea` about 80 ms to about 58 ms per op; `RCombineImages` (RGB over RGB) about 12 ms to about 4 ms per op. |
| kqueue-backed event loop instead of `select()` | this fork (DAR-431) | Done on QEMU, merged | No speed change by itself (see below); it gives one wait that the timer and file-watch tickets build on. |
| Monotonic, coalesced timers (`NOTE_MACHTIME` + `NOTE_LEEWAY`) | this fork (DAR-432) | Done on QEMU | Timers survive `settimeofday()` steps. Idle wake-ups about 1.0 per second to about 0.35 per second; idle CPU about 0.9 s to about 0.35 s per 60 s (see below). |
| `EVFILT_VNODE` watch of `~/GNUstep/Defaults` instead of 2 s `stat()` polling | this fork (DAR-433) | Done on QEMU | Idle wake-ups about 0.35 per second to about 0.03 per second; idle CPU about 0.4 s to about 0.06 s per 60 s; a changed defaults file is reloaded within about 0.2 s (see below). |
| Main threads of Xorg and wmaker at `QOS_CLASS_USER_INTERACTIVE` | this fork, `src/main.c` (branch `dar-439-qos`), and the puredarwingop Xorg driver (PureDarwin fork branch `dar-439-qos`) (DAR-439 step 2) | Built, QEMU-measured, real Pi pending | Base priority 31 to 37 (not 46). QEMU, one core, under CPU hogs: 95th percentile window-move round trip 140-154 ms to 41-45 ms (4 hogs), 43 ms to 27-31 ms (1 hog). Idle median round trip rose about 11.5 ms to 14-19 ms. See section 6. |
| Launch programs with `posix_spawnp` and watch exits with `EVFILT_PROC` instead of fork+exec and SIGCHLD | this fork, `src/launch.c` (branch `dar-434-spawn`) (DAR-434) | Done on QEMU, real Pi pending | wmaker is blocked about 26 ms per launch instead of about 100 ms (QEMU); fork+exec cost grows with the parent (0 / 64 / 128 MB parent: 45 / 640 / 1210 ms of parent CPU per launch), spawn stays at about 11 ms. Children no longer inherit wmaker's descriptors. See section 7. |

### 1. Session priority (biggest user-visible win)

The launchd in this port is built for the iPhoneOS SDK (`TARGET_OS_EMBEDDED`), and PureDarwin's
launchd runs a job with no `ProcessType` as `DAEMON_BACKGROUND` (`launchd core.c:2257`). On the
Pi, `ps -o pri` showed xdm, Xorg, wmaker and every app they started at scheduler priority 4
(the Background band), while launchd and configd were at 37. Background also brings the larger
timer coalescing of the background QoS tier. Adding `ProcessType` = `Interactive` to the job
(mapped at `core.c:2737-2739`; children inherit it) fixed it. After that change the UI was much
faster, and Window Maker's Kill, which had seemed not to work, worked. Still to do for that
ticket: raise the wmaker and Xorg main threads to `QOS_CLASS_USER_INTERACTIVE` themselves.

### 2. `-O2`

Everything in the X11 stack used to be compiled at `-O0` (inherited from another port's build
recipe). It now defaults to `-O2`, with `-D_FORTIFY_SOURCE=0`, `-fno-stack-protector` and
`-fno-builtin` kept (the last so `memcpy` still calls the real arm64 routine in
libsystem_platform; checked in the generated assembly). Xorg and Xvfb were also `-O0` because
the meson `--buildtype=plain` setting passes no optimisation flag; they now get `$X11_OPT`
through the cross-file. Nothing had to stay at `-O0`.

Micro-benchmark (`x11_bench`, `tools/userland_staging/x11_bench.c` in xnu-iokit-pi3), **QEMU**,
minimum of 3 runs, ms per op:

| Benchmark | `-O0` | `-O2` |
|---|---|---|
| RScaleImage | 39 | 25 |
| RCombineImages | 20 | 12 |
| RBlurImage | 47 | 26 |
| RBevelImage | 1.2 | 0.77 |
| RConvertImage + XCopyArea | 98 | 78 |
| 2000 x XFillRectangle/XDrawLine/XDrawString + sync | 703 | 428 |
| RSmoothScaleImage | 730 | 628 |

Gradients and `XGetImage` did not change. `RSmoothScaleImage` is still slow; `libsystem_m` is
still built at `-O0` and is the probable cost (not yet checked).

### 3. wrlib fast path (this fork)

Commit `1ef1522c`. The ticket assumed the default dithered renderer ran for every image, but
`wrlib/context.c:632-634` already forces `RBestMatchRendering` at depth 24 and above, so the
hot path is the BestMatch loop that called `XPutPixel` once per pixel. Changes:

* `convert.c`, `image2TrueColor`: for a 32bpp ZPixmap visual with 8-bit masks, write pixels
  straight into `ximg->image->data`, honouring `bytes_per_line` and `byte_order` (following
  libX11 `src/ImUtil.c:727-750`). NEON (`vld3q/vld4q` + `vst4q`) for byte-aligned layouts, a
  scalar fallback for the rest. Every other visual still takes the generic path.
* `raster.c`: `RCombineImages`, `RCombineImagesWithOpaqueness` and `RCombineArea` use NEON
  blends with the same `(d*c + s*a)/256` arithmetic as before.
* `gradient.c`: `renderGradientWidth` uses `vst3q` fills.
* New `wrlib/fastpath.h`. NEON is compiled only under `__ARM_NEON`; `-DWR_NO_NEON` builds the
  scalar versions.

Correctness: a pixel-diff test (`tools/userland_staging/wrlib_fastpath_test.c` in
xnu-iokit-pi3) compiles the real `convert.c`, `raster.c` and `gradient.c` on an arm64 host and
compares the output with the pristine 0.96.0 files byte for byte, for the NEON build and for
`-DWR_NO_NEON`. It covers six channel layouts, both byte orders, both render modes, row padding
of 0, 4 and 12 bytes, widths from 1 to 257, gradients and all the combine functions (about
5.2 MB compared; identical). The test was checked to fail when one NEON lane is swapped. In the
guest under Xvfb, `x11_bench`'s `CONVCHECK` reports zero mismatches.

Speed, **QEMU**, before and after this change only (both `-O2`):

| Benchmark | Before | After |
|---|---|---|
| RConvertImage + XCopyArea | 75-85 ms/op | 55-63 ms/op |
| RCombineImages (opacity, RGB over RGB) | 12.4 ms/op | 3.9 ms/op |
| Gradients, RGBA-source blends | no clear change | no clear change |

Open issue: in the full-NEON build `RScaleImage` went from about 27 to 46-57 ms/op and
`RBlurImage` from about 26 to about 40 ms/op on QEMU, although neither function was changed and
their instruction counts are identical. A memory-layout effect in QEMU is suspected but not
proven. This needs a measurement on the real Pi; if it reproduces there, build the affected
paths with `-DWR_NO_NEON`.

### 4. kqueue event loop (this fork)

Commits `e2baf4a5` and `347926fc`. `WINGs/handlers.c` (`W_HandleInputEvents`) now blocks in one
lazily created kqueue, registered level-triggered (no `EV_CLEAR`). The X connection is
registered on first use; read, write and except masks map to `EVFILT_READ`, `EVFILT_WRITE` and
`EVFILT_EXCEPT` (`NOTE_OOB`); the timeout still comes from the same timer queue. `select()` stays
as a runtime fallback: it is used if `kqueue()` fails, if a registration fails for any reason
other than `EBADF`, or when the environment has `WM_EVENT_BACKEND=select`. A `pthread_atfork`
child hook recreates the kqueue after `fork()` (xnu closes kqueue fds on fork,
`kern_event.c:3025`). `WM_EVENT_STATS=N` prints wait counters every N seconds. Extra filters are
added through `W_KQueueAddFilter`, which is what the timer (DAR-432) and file-watch (DAR-433)
changes will use.

Window Maker registers no WINGs input handlers of its own, so the only file descriptor it waits
on is the X connection. `kevent()` is not restarted after a signal, even with `SA_RESTART`
(`kern_event.c:7251`), which matches `select()`, so the SIGCHLD and SIGUSR handlers still
interrupt the wait.

**QEMU**, same image and same Xvfb, four warmed runs (select, kqueue, select, kqueue); the two
backends are indistinguishable:

| Measure | select | kqueue |
|---|---|---|
| Idle wmaker CPU time over 60 s | 0.92 s, 0.85 s | 0.94 s, 0.87 s |
| Idle blocking waits | about 1 per second | about 1 per second |
| Round-trip latency, median | 10.6 to 11.7 ms | 10.6 to 11.1 ms |
| 200-event XTest burst | 3.8 to 6.2 ms per event | 3.7 to 5.9 ms per event |

Idle CPU is timer-driven (every idle wait is a timeout; none is an input wakeup), and nothing
spins. Also observed, not investigated: the first wmaker started after a fresh Xvfb answers
about 25 times slower (median 275 ms) on either backend.

One known limit: an fd that is closed without being deleted from the handler list never fires
under kqueue (under `select()` it spins).

### 5. Monotonic, coalesced timers (this fork)

Commits `def4898a` and `8a5e481e` (`WINGs/handlers.c`). Two changes.

**Clock.** WUtil timer deadlines were `gettimeofday()` timevals. chronyd steps the clock with
`settimeofday()` on this port (DAR-169), so a step back left every pending timer waiting for the
step size and a step forward fired them all at once. Deadlines are now timevals on
`mach_absolute_time()` (`CLOCK_MONOTONIC` on other systems). That is the clock the kernel's own
`kevent()` timeout runs on (`kern_event.c:7932-7957`, `kevent_legacy_get_deadline` converts through
`clock_absolutetime_interval_to_deadline`) and the epoch of `NOTE_MACHTIME|NOTE_ABSOLUTE` deadlines
(`bsd/sys/event.h:568-576`). The queue semantics (`WMAddTimerHandler`,
`WMAddPersistentTimerHandler`, `WMDeleteTimerHandler`) are unchanged, and the select fallback keeps
the same queue with precise deadlines.

**Coalescing.** New `WMAddTimerHandlerWithLeeway` and `WMAddPersistentTimerHandlerWithLeeway`; the
old calls use a default leeway of 10% of the interval, at most 50 ms. Under the kqueue backend the
whole queue is one `EVFILT_TIMER` knote, armed for the next wake-up window: deadline = earliest
deadline, leeway = (earliest `deadline + leeway` over all pending timers) minus that deadline, so no
timer is held past its own leeway. `kevent()` then blocks with no timeout; on each wake-up every
timer whose deadline has passed fires, so timers that fall in one window share one wake-up. What the
kernel does with it:

* `NOTE_MACHTIME|NOTE_ABSOLUTE`: `data` is an absolute deadline in `mach_absolute_time` units
  (`filt_timervalidate`, `kern_event.c:1337, 1372`). Absolute timers are forced one-shot
  (`filt_timerattach`, `kern_event.c:1629-1631`), so the knote is re-armed after each delivery; a
  deadline in the past fires at once (`filt_timer_is_ready`). Changing it is a touch of the same
  knote (`filt_timertouch`, `kern_event.c:1668-1695`).
* `NOTE_LEEWAY`: `ext[1]` is the leeway in the same units (`kern_event.c:1348-1363`).
  `filt_timerarm` passes it to `thread_call_enter_delayed_with_leeway` (`kern_event.c:1574`), which
  uses `max(leeway, default slop for the thread's QoS tier)` as the slop and sets the hard deadline
  to deadline + slop (`thread_call.c:1236-1246`); the timer call is armed with that leeway
  (`thread_call.c:799-804`), so the CPU pops at the hard deadline unless something else wakes it
  first and then everything whose deadline has passed fires.
* The plain `kevent()` timeout cannot be used for this: `kqueue_scan` waits with
  `TIMEOUT_NO_LEEWAY` (`kern_event.c:7510-7512`). It stays as the path if the knote cannot be
  registered.
* `ext[1]` exists only in `struct kevent64_s` / `kevent_qos_s`; legacy `kevent()` zero-fills it.
  `kq64()` calls the real `kevent64()` (`syscalls.master:560`, number 369), which `libsystem_kernel`
  exports since DAR-456 (before that `kq64()` issued the syscall by hand: number in `x16`, `svc #0x80`;
  that raw syscall is gone). A kqueue is "legacy32" or not from its first use, and
  xnu refuses the other interface on it with `EINVAL` (`kern_event.c:6873-6877`), which the first
  version of this change ran into (the timer never armed); the whole WUtil kqueue (waits,
  registrations, timer) goes through `kq64()`, and the kqueue backend is compiled for arm64
  Darwin only. `libWUtil` therefore needs the DAR-456 `libsystem_kernel` on the target.

Which timers are alive on an idle Window Maker (`WM_EVENT_STATS` now also counts callback fires):
exactly two, each about every 2 s: `wDefaultsCheckDomains` (`src/defaults.c:1171`, the defaults
poll that DAR-433 will remove) and `synchronizeUserDefaults` (`WINGs/userdefaults.c:167`). They
ran out of phase, so every second had a wake-up. Both now get 1000 ms of leeway
(`DEFAULTS_CHECK_LEEWAY`, `UD_SYNC_LEEWAY`): a 2 s poll that runs up to 1 s late is harmless, and
their windows overlap, so after the first shared wake-up they stay together (each re-arms from the
moment it fired). Side effect: the pair now runs every 3 s instead of every 2 s (the kernel fires
at the hard deadline, then the timers re-arm), which only makes the defaults-change poll slower.
Everything else (balloon, clip auto-raise, menu and scroll timers) keeps the default leeway of at
most 50 ms.

**Clock-step test** (`tools/userland_staging/wutil_timer_test.c` in xnu-iokit-pi3, run as root in
the guest, durations measured on `mach_absolute_time`). A 600 ms timer is armed, the wall clock is
stepped after 150 ms, the clock is restored afterwards:

| Case | Before (wall-clock queue) | After |
|---|---|---|
| step back 60 s, 600 ms timer | not fired within 3000 ms (would wait about 60 s) | fires at 624-703 ms |
| step forward 60 s, 600 ms timer | fires at 196-263 ms (early) | fires at 640-704 ms |
| persistent 100 ms timer, 1 s window, step back 60 s in the middle | 3-4 fires (5-6 without a step) | 5 fires (5 without a step) |

Under a running wmaker (`WM_EVENT_STATS=10`), a `-3600 s` step stopped the stats output for about
60 s before (the wait was blocked on the old deadline), and after it the wait counter kept
advancing. The same test passes under `WM_EVENT_BACKEND=select`, and `wutil_kq_test` (the DAR-431
cases, including the `W_KQueueAddFilter` one, now on `kevent64`) still passes on both backends.

**Idle wake-ups and CPU**, **QEMU**, one Xvfb per boot, a discarded warm-up wmaker, then two 60 s
idle windows (`tools/wm_timer_verify_guest.sh idle`), two boots before and three after; indicative
only (TCG):

| Measure | Before (DAR-431 loop, wall-clock timers) | After |
|---|---|---|
| Blocking waits at idle | about 1.0 per second (10 per 10 s) | about 0.35 per second (4 per 11.5 s) |
| Idle wmaker CPU over 60 s | 0.87, 0.94, 0.83, 0.96 s | 0.37, 0.42, 0.32, 0.36, 0.36, 0.33 s |
| Round-trip latency, median | 12.2, 12.9 ms | 10.3, 11.2, 12.2, 10.6 ms |

The latency difference is noise; there is no sign of a cost from the wider timer window. The
warm-up wmaker of each boot is not comparable (it is still starting). The kernel-side effect of
leeway in the isolated test is visible but noisy: in `wutil_timer_test` case 5 (timer A 300 ms with
400 ms leeway, timer B 600 ms) A fired together with B at about 600-650 ms (one wake-up) in two of five runs and at
about 400 ms (another wake-up at that time) in the others, so in practice coalescing depends on
what else wakes the CPU; the idle numbers above are the measurement that counts.

Caveats: the leeway knob is only as good as the kernel's coalescing (per-CPU timer queue; other
timers may wake the CPU earlier, which fires our timers early but never before their deadline).
The select fallback ignores leeway. The numbers are QEMU only; the real Pi is still to be
measured.

### 6. Defaults files watched with `EVFILT_VNODE` (this fork)

Commits `ee0d4ae8` (the change), `5cea924f` (a bug it exposed), `77e15049` and `3fcdbf12`
(`WM_DEFAULTS_TRACE`, see below). Darwin has no inotify, so `HAVE_INOTIFY` stays off and
`wDefaultsCheckDomains` used to run from a 2 s timer that `stat()`ed three files
(`src/defaults.c`: `WindowMaker`, `WMWindowAttributes`, `WMRootMenu`) and compared `st_mtime`
(`WMState` and the global `/usr/X11/etc/WindowMaker/*` files were never polled, and still are not).

**What it does now.** `wDefaultsStartWatching()` (`src/defaults.c`, called from `src/startup.c`
where the poll was armed) registers `EVFILT_VNODE` knotes on the WUtil kqueue through
`W_KQueueAddFilter` (DAR-431): one on the Defaults directory (`NOTE_WRITE|NOTE_DELETE|NOTE_RENAME|
NOTE_LINK|NOTE_REVOKE`: entries created, removed or renamed, which is what the atomic
temp-file + `rename()` of `WMWritePropListToFile`, `WINGs/proplist.c:1713`, does) and one on each
of the three files (`NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK|NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE`:
in-place writes and touch). Descriptors are opened `O_EVTONLY` (`bsd/sys/fcntl.h:138`) with
`O_CLOEXEC`, knotes are `EV_ADD|EV_CLEAR`. Events are debounced by 100 ms (a rename is preceded by
the creation of the temp file, and an in-place writer produces several events). Then every watched
path is `stat()`ed again and re-opened if its inode changed (a file knote is bound to its vnode
and reports the delete or rename once), and the existing domain check runs. If the Defaults
directory does not exist yet it is created (wmaker creates it on its first save anyway,
`proplist.c:1652`).

**Safety nets.** A 30 s poll (`DEFAULTS_SAFETY_INTERVAL`, `src/wconfig.h.in`; leeway 10 s) stays in
case some filesystem delivers no vnode events. Under the select backend
(`WM_EVENT_BACKEND=select`, or `kqueue()` failing) `W_KQueueAddFilter` returns NULL, nothing is
watched and the old 2 s poll (with its DAR-432 leeway) runs as before; tested.

**What the kernel guarantees.** The filter is VFS-generic: `vnode_filtops` (`bsd/vfs/vfs_vnops.c:148`,
registered at `kern_event.c:345`) attaches to any vnode (`vfs_vnops.c:1865-1888`), and the events are
posted by the VFS layer above the filesystem, in `kpi_vfs.c`: `NOTE_WRITE` after `VNOP_WRITE`
(`:3721`), `NOTE_ATTRIB` after `VNOP_SETATTR` (`:3637`), `NOTE_DELETE|NOTE_LINK` after
`VNOP_REMOVE` (`:4066`), `NOTE_RENAME` / `NOTE_DELETE` of the replaced target in `VNOP_RENAME`
(`:4523-4526`), and `NOTE_WRITE` on the parent for every directory-entry change (`:3297`, `:4067`,
`:4175`). So HFS+ and msdosfs need no support of their own; the filesystem is only asked for remove
notifications for network filesystems (`vfs_vnops.c:1890`). Checked on this port's HFS+ root, below.

**Vnode events fire on HFS+.** `wutil_vnode_test` (`tools/userland_staging/wutil_vnode_test.c` in
xnu-iokit-pi3, run in the guest as root; it uses the same open/register calls as wmaker) on `/var/root`
(the HFS+ root, `rd=disk0s2`) and on `/tmp` (also on that root volume; the image has no other
filesystem mounted there), **QEMU**:

| Case | File watch | Directory watch | Delay (incl. test's 2 ms poll and `sync`) |
|---|---|---|---|
| A in-place write (append) | `WRITE EXTEND` | none | 50-62 ms |
| D touch (`utimes`) | `ATTRIB` | none | 0-2 ms |
| B atomic replace (temp file + `rename`) | `DELETE` (old vnode unlinked) | `WRITE` | 57-67 ms |
| E in-place write after re-opening the new file | `WRITE EXTEND` | none | 50-51 ms |
| C delete + recreate | `DELETE LINK` | `WRITE` | 234-256 ms |

All pass on both directories. A rename-replace reports `DELETE` on the file watch and not
`RENAME` (the renamed vnode is the temp file, not the watched one), so a watch on the file alone
would go stale after the first replace; the directory watch plus re-opening is what keeps it working.
msdosfs was not tested (no msdosfs volume is mounted in the guest); by the code above it gets the same
VFS-level events.

**Reload on QEMU** (`tools/wm_vnode_verify_guest.sh reload`; Xvfb + wmaker; the domain files exist at
start). With the root menu open, `MenuTitleBack` of `WindowMaker` is changed and a snapshot of the
framebuffer is taken 1 s later (the change itself takes about 1.5 s in the guest, `mv` and `touch` are
slow under TCG); the title colour of the open menu shows whether the change was applied:

| Change | Before (2 s poll, 1 s leeway) | After (vnode watch) |
|---|---|---|
| A atomic rename-replace, red | new colour | new colour |
| B in-place write, green | new colour | new colour |
| C delete + recreate, blue | **old colour** (applied after the snapshot) | new colour |
| D touch only | (reloaded later) | reloaded, no visible change |

Three samples, so this is only an illustration of the poll's random 0-3 s delay; the trace
(`WM_DEFAULTS_TRACE=1` prints a line for every vnode event and every reload, with millisecond
time) shows the watch path: first event to reload takes about 100-200 ms in every case (debounce plus
scheduling). Also tried and passing: starting with no Defaults files at all (the files are created
after wmaker is running and are picked up), and `WM_EVENT_BACKEND=select` (reloads via the 2 s poll, no
vnode events). Reload of `WMRootMenu` is confirmed by the trace (`reload WMRootMenu` after each change)
but not on screen: the root menu that x11_probe's click opens stays open and does not close again in
this harness, so a rebuilt menu was never shown. `OpenRootMenu` also only rebuilds the menu when the
file's mtime is a newer second than the one on screen (`src/rootmenu.c:1620-1621`, unchanged).

**Idle wake-ups and CPU**, **QEMU**, one Xvfb per boot, a discarded warm-up wmaker and two 60 s idle
windows per boot (`tools/wm_vnode_verify_guest.sh idle`), same image recipe before and after;
indicative only (TCG):

| Measure | Before (DAR-432 state) | After |
|---|---|---|
| Blocking waits at idle | 4 per 11 s, about 0.35 per second (`wDefaultsCheckDomains` and `synchronizeUserDefaults` fire together every 3 s) | 1 per 30 s, about 0.03 per second (only the safety poll) |
| Idle wmaker CPU over 60 s | 0.39, 0.40, 0.43 s | 0.03, 0.07, 0.07 s |

The before numbers agree with DAR-432's (0.32-0.42 s). `synchronizeUserDefaults`
(`WINGs/userdefaults.c:167`) was the second idle timer: wmaker's own domain is `dontSync`, so it never
did anything; it is now only armed when a database that syncs exists (`WMEnableUDPeriodicSynchronization`
arms it too), which is what Linux builds get with inotify.

**Other changes.**

* Change detection also compares inode and size, not only `st_mtime` (one second resolution): with a
  check right after the first write, a second change in the same second was invisible. Seen in the
  test: a truncate-then-write saw the empty file first (`could not load domain WMRootMenu`), and the
  next event reloaded the complete one.
* `wDefaultsCheckDomains` no longer re-arms the poll timer itself: every SIGHUP or `Reconfigure`
  message used to add another poll chain. The poll is now a separate callback.
* `wDefaultsInitDomain` read `stbuf.st_mtime` uninitialised when the user file did not exist, which
  could set the domain's timestamp to stack garbage and make the change check ignore the file
  once it appeared (seen: a `WindowMaker` file created after start never reloaded). `stbuf` is zeroed.

**Known limits.** The global defaults (`/usr/X11/etc/WindowMaker`, `GLOBAL_DEFAULTS_SUBDIR`) were never
polled (they are read only when the user file changes) and are not watched; `WMState` likewise. One
open descriptor per watched path (four). Needs the kqueue backend (arm64 Darwin, through `kq64()`,
see section 5); elsewhere the poll stays. The guest's wall clock is stepped by chronyd (DAR-169), so
the test script stops it and sets file mtimes itself; real use is not affected by that, but as
before a file with an mtime older than the last loaded one is only noticed if its inode or size
differ. msdosfs and the real Pi are not measured.

### 6. Main-thread QoS (DAR-439 step 2)

Branch `dar-439-qos`. `main()` calls `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`
first thing (Darwin only). Xorg's main thread does the same from the puredarwingop driver's module
setup (`PDGOPSetup`, which runs on the main thread before probing), which logs
`puredarwingop: main thread QoS set: rc=0, class now 0x21` to Xorg.0.log. Apps are not touched.

**What the kernel does** (xnu-7195). The ticket expected base priority 46 (`thread_policy.c:74`,
`sched.h:161`). That is the table value, but the session job is a DAEMON_INTERACTIVE task, and for
daemons `task_policy.c:866-868` caps the task's QoS at USER_INITIATED, applied to every thread at
`thread_policy.c:1554-1556`. So the thread goes from 31 (LEGACY, the daemon primordial QoS,
`task_policy.c:2069-2074`) to 37 (`sched.h:162`). Measured with `thread_info` in the guest:
`base_pri=31` before, `37` after, `pthread_get_qos_class_np` 0x15 to 0x21. A process cannot get 46
from this job type: `ProcessType=App` was tried and only the job's own first process gets the app
priority (47), its children (Xvfb, wmaker) are back at 31 and wmaker's own QoS call left it at 31.

QoS set before `exec()` is lost (`kern_exec.c:4033`, `task_set_main_thread_qos`), so it has to be
done inside the process.

The timer claim holds on paper and cannot be seen on QEMU. `tcoal_qos_adjust`
(`timer_call.c:1690`) maps USER_INTERACTIVE to latency tier 0 and LEGACY to tier 1
(`thread_policy.c:106-109`); `arm_timer.c:279-285` gives tier 0 a coalescing window of at most 1 ms
(shift 3) and tier 1 at most 5 ms (shift 2). `usleep(2000)` overshoots by about 2.1-2.3 ms with or
without the QoS call on QEMU, so the emulated clock tick hides it.

**QEMU measurements** (one core, `tools/wm_qos_verify_guest.sh`: Xvfb and wmaker started by a
launchd job with `ProcessType=Interactive`; `wmlat rt 200`, round trip of an `XMoveWindow` through
Xvfb and wmaker back to the client, median / 95th percentile in ms; the client stays at 31; two or
three boots each, boot-to-boot noise is several ms):

| Load | neither raised | only Xvfb | only wmaker | both |
|---|---|---|---|---|
| idle | 11.4 / 13.6 to 12.0 / 17.9 | 12.5 / 19.8 | 15.6 / 26.2 to 19.5 / 28.4 | 14.0 / 17.2 to 19.0 / 24.5 |
| 1 `yes` | 11.5 / 43 | 13.2 / 31 | 17.5 / 40 to 20.6 / 45 | 15.7 / 27 to 19.8 / 31 |
| 4 `yes` | 11.8 / 140 to 12.3 / 154 | 12.2 / 80 | 16.5 / 114 to 22.8 / 118 | 15.9 / 41 to 19.9 / 43 |
| 1 `yes` at nice 20 | 11.1 / 42 to 11.5 / 46 | 10.8 / 30 | 19.6 / 36 to 21.9 / 48 | 15.9 / 26 to 19.6 / 33 |
| 1 `yes` at USER_INTERACTIVE (37) | 153 / 265 | 295 / 392 | not run | 36 / 71 to 40 / 105 |

Raising both cuts the tail under load by 40 to 70 percent and keeps a busy 37-priority peer from
starving the UI; raising only one is worse than both (only wmaker: worst idle cost, small gain),
which is the priority inversion the ticket warned about. Cost: the idle median is 3 to 7 ms
higher (about 25 to 60 percent) with both raised. Cause not found; the test
client stays at 31, and the loaded tail improves in the same runs. The real Pi is not measured;
check `ps -o pid,pri,comm` (Xorg and wmaker 37, apps 31) and Xorg.0.log.

### 7. `posix_spawnp` launches and `EVFILT_PROC` child exits (DAR-434, this fork)

**What changed.** `src/launch.c` has `wSpawn()`, used by `ExecuteShellCommand`, the relaunch path
(`RelaunchWindow`), dock/clip launches (`dock.c execCommand`), the `wmsetbg` helper
(`start_bg_helper`) and session restore. It calls `posix_spawnp` with `POSIX_SPAWN_SETSID |
POSIX_SPAWN_CLOEXEC_DEFAULT` (xnu `bsd/sys/spawn.h`; `kern_exec.c:2533-2575` marks only the
descriptors named in the file actions inheritable, `:3749` is SETSID). Only stdin/stdout/stderr
(`addinherit_np`, or `adddup2` for the helper's stdin pipe) reach the child; `SetupEnvironment()`'s
variables (DISPLAY for multihead, `WRASTER_COLOR_RESOLUTION`) go in an envp. The helper gets a
UTILITY clamp. Also fixed on the way, first and on its own: the relaunch path's
`malloc(argc + 1)` (bytes, not pointers) heap overflow, commit `b7c3afc2`.

`posix_spawnp` comes from this port's libsystem_c (`build_libsystem_c.sh` compiles Libc
`sys/posix_spawn.c:70`): PATH search and `execvp`'s ENOEXEC fallback to `/bin/sh` (`:142-154`).
Plain `posix_spawn` does not do the fallback (it returns ENOEXEC); `tools/userland_staging/
spawn_test` checks both.

**Reaping.** Each child gets an `EVFILT_PROC` knote (`NOTE_EXIT | NOTE_EXITSTATUS`, `EV_ONESHOT`)
on the WUtil kqueue (`W_KQueueAddFilter`); `data` is the wait status. The callback does
`waitpid(pid, WNOHANG)`, queues the status with `NotifyDeadProcess()` and wakes the main loop with a
ClientMessage on wmaker's own window, so the death handlers run in `DispatchEvent()` as before.
Not from a timer: death handlers open modal dialogs, and WUtil's timer loop is not re-entrant
(a first version used a zero-delay timer and wmaker died with SIGBUS after the error dialog was
dismissed). xnu raises `NOTE_EXIT` before the process is a zombie (`kern_exit.c:1579` vs `:1642`
`SZOMB` and `:1648` SIGCHLD), so the `waitpid` in the callback usually finds nothing; the SIGCHLD
handler (`startup.c buryChild`, still installed) reaps it a moment later. Handlers fire once either
way (a handler is removed when it runs). Children not spawned through `wSpawn()` and the select
backend rely on SIGCHLD alone, as before. A failed spawn (no such program) is reported at once; the
dock shows the same "Could not execute command" dialog the old exit status 111 produced.
`WM_SPAWN=fork` selects the old fork+exec at run time; `WM_SPAWN_TRACE=1` logs launches and deaths.

**QEMU results** (one core; QEMU timings are indicative only; the host was shared):

| Measure | fork+exec | posix_spawn |
|---|---|---|
| wmaker blocked in the launch call (wmaker, 8 MB RSS), per launch | 76-155 ms, about 100 ms | 20-40 ms, about 26 ms |
| `spawn_test bench`, parent holding 0 / 64 / 128 MB: ms per launch (child `exit 0`) | 668 / 1414 / 2058 | 622 / 604 / 613 |
| same: parent CPU per launch | 45 / 641 / 1214 ms | 11.5 / 11 / 11 ms |

`spawn_test` passes all its checks: SETSID, CLOEXEC_DEFAULT with and without `addinherit_np`, `adddup2`,
envp, PATH search, ENOENT/EACCES/ENOEXEC, `EVFILT_PROC` status for an exit code and for SIGKILL
(about 41 ms from kill to event), ESRCH on an already reaped pid. Under Xvfb (`tools/
wm_spawn_verify_guest.sh e2e spawn|fork`): menu launches, dock AutoLaunch, `WindowRelaunchKey`
relaunch, kill of a long-running child (reported by the kqueue path, status 0xf, no zombies), a child
sees only fds 0 and 2 plus its redirect. Children and wmaker show the same thread base priority
(`qos_probe`); `posix_spawnattr_set_qos_class_np` only accepts UTILITY/BACKGROUND/MAINTENANCE
(libpthread `qos.c:571`; they are ceilings), so user launches keep the default and nothing is boosted.

**Not measured / caveats.** The real Pi. The end-to-end launch latency through the menu (child's first
instruction minus the call) is about 2 s on QEMU, almost all `/bin/sh` and dyld start-up, so it does not
compare the two; and the fork variant of that harness (`wm_spawn_verify_guest.sh lat fork`) never got a
menu click through on QEMU (the fork e2e run did), cause not found. `util/wmsetbg` is not built here,
so the helper launch is untested (its error path is the same code). Session restore now also starts
apps in their own session.

### Not yet measured on real hardware

All of the `-O2` and wrlib numbers above are from QEMU. The Pi measurements (run `x11_bench :0 5`
under Xvfb on the Pi, plus a subjective check of menus, window moves and Terminal scrolling)
are still to be done, and this file will be updated when they are.
