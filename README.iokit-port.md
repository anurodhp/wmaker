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
| `EVFILT_VNODE` watch of `~/GNUstep/Defaults` instead of 2 s `stat()` polling | this fork (DAR-433) | Planned, after DAR-431 | |

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
  This port's `libsystem_kernel` exports neither `kevent64` nor `kevent_qos`, so `kq64()` issues the
  syscall directly (`syscalls.master:560`, number 369; arm64 Darwin: number in `x16`, `svc #0x80`,
  carry set = error with errno in `x0`, `bsd/dev/arm/systemcalls.c:305-307`). A kqueue is "legacy32" or not from its first use, and
  xnu refuses the other interface on it with `EINVAL` (`kern_event.c:6873-6877`), which the first
  version of this change ran into (the timer never armed); the whole WUtil kqueue (waits,
  registrations, timer) now goes through `kq64()`, and the kqueue backend is compiled for arm64
  Darwin only. The clean fix would be exporting `kevent64` from `libsystem_kernel`; not done here.

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
idle windows (`tools/wm_timer_verify_guest.sh idle`), two boots each; indicative only (TCG):

| Measure | Before (DAR-431 loop, wall-clock timers) | After |
|---|---|---|
| Blocking waits at idle | about 1.0 per second (10 per 10 s) | about 0.35 per second (4 per 11.5 s) |
| Idle wmaker CPU over 60 s | 0.87, 0.94, 0.83, 0.96 s | 0.37, 0.42, 0.32, 0.36 s |
| Round-trip latency, median | 12.2, 12.9 ms | 10.3, 11.2 ms |

The latency difference is noise; there is no sign of a cost from the wider timer window. The
warm-up wmaker of each boot is not comparable (it is still starting). The kernel-side effect of
leeway in the isolated test is visible but noisy: in `wutil_timer_test` case 5 (timer A 300 ms with
400 ms leeway, timer B 600 ms) A fired together with B at about 600 ms in one of four runs and at
about 400 ms (another wake-up at that time) in the others, so in practice coalescing depends on
what else wakes the CPU; the idle numbers above are the measurement that counts.

Caveats: the leeway knob is only as good as the kernel's coalescing (per-CPU timer queue; other
timers may wake the CPU earlier, which fires our timers early but never before their deadline).
The select fallback ignores leeway. The numbers are QEMU only; the real Pi is still to be
measured.

### Not yet measured on real hardware

All of the `-O2` and wrlib numbers above are from QEMU. The Pi measurements (run `x11_bench :0 5`
under Xvfb on the Pi, plus a subjective check of menus, window moves and Terminal scrolling)
are still to be done, and this file will be updated when they are.
