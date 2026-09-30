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
| Monotonic, coalesced timers (`NOTE_MACHTIME` + `NOTE_LEEWAY`) | this fork (DAR-432) | Planned, after DAR-431 | |
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

### Not yet measured on real hardware

All of the `-O2` and wrlib numbers above are from QEMU. The Pi measurements (run `x11_bench :0 5`
under Xvfb on the Pi, plus a subjective check of menus, window moves and Terminal scrolling)
are still to be done, and this file will be updated when they are.
