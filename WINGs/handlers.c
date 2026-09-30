
/*
 * WINGs internal handlers: timer, idle and input handlers
 */

#include "wconfig.h"
#include "WINGsP.h"

#include <sys/types.h>
#include <unistd.h>

#include <X11/Xos.h>

#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#include <time.h>

#ifdef __APPLE__
# include <mach/mach_time.h>
# include <dlfcn.h>
#endif

/*
 * The kqueue backend talks to the kernel through kevent64() (see kq64()
 * below), which needs the arm64 Darwin syscall ABI; anywhere else
 * WM_USE_KQUEUE is ignored and the select() path is used.
 */
#if defined(WM_USE_KQUEUE) && !(defined(__APPLE__) && defined(__arm64__))
# undef WM_USE_KQUEUE
#endif

#ifdef WM_USE_KQUEUE
# include <sys/event.h>
# include <sys/syscall.h>
# include <fcntl.h>
# include <errno.h>
# include <string.h>
# include <pthread.h>
# if defined(NOTE_MACHTIME) && defined(NOTE_LEEWAY)
#  define WM_KQ_TIMER		/* EVFILT_TIMER with NOTE_MACHTIME|NOTE_LEEWAY, see kq_timer_arm() */
# endif
#endif

#ifndef X_GETTIMEOFDAY
#define X_GETTIMEOFDAY(t) gettimeofday(t, (struct timezone*)0)
#endif

typedef struct TimerHandler {
	WMCallback *callback;	/* procedure to call */
	struct timeval when;	/* when to call the callback */
	void *clientData;
	struct TimerHandler *next;
	int nextDelay;		/* 0 if it's one-shot */
	int leeway;		/* ms after `when` this timer may be late, for coalescing */
} TimerHandler;

typedef struct IdleHandler {
	WMCallback *callback;
	void *clientData;
} IdleHandler;

typedef struct InputHandler {
	WMInputProc *callback;
	void *clientData;
	int fd;
	int mask;
} InputHandler;

/* queue of timer event handlers */
static TimerHandler *timerHandler = NULL;

static WMArray *idleHandler = NULL;

static WMArray *inputHandler = NULL;

#define timerPending()	(timerHandler)

/*
 * Timer deadlines are kept as timevals on a MONOTONIC clock, not on the
 * wall clock (DAR-432). chronyd calls settimeofday() on this port (DAR-169)
 * and a wall-clock deadline then fires early (forward step) or waits for
 * the step size (backward step). The Darwin clock is mach_absolute_time():
 * it is the clock the kernel's own kevent()/select() timeouts run on
 * (kern_event.c kevent_get_timeout -> clock_absolutetime_interval_to_deadline)
 * and the epoch of EVFILT_TIMER's NOTE_MACHTIME|NOTE_ABSOLUTE deadlines
 * (sys/event.h:575). Elsewhere CLOCK_MONOTONIC, and gettimeofday() only when
 * neither exists. The value is never {0,0}: IS_ZERO() marks a timer that is
 * running its callback.
 */
#ifdef __APPLE__
static mach_timebase_info_data_t mono_tb;

static void monoInit(void)
{
	if (mono_tb.denom == 0)
		mach_timebase_info(&mono_tb);
}

/* a * n / d without a 128-bit intermediate (libgcc's __udivti3 is not linked here) */
static uint64_t mulDiv(uint64_t a, uint32_t n, uint32_t d)
{
	return (a / d) * n + (a % d) * n / d;
}

static uint64_t absToNs(uint64_t a)
{
	monoInit();
	return mulDiv(a, mono_tb.numer, mono_tb.denom);
}
#endif

static void rightNow(struct timeval *tv)
{
#ifdef __APPLE__
	uint64_t ns = absToNs(mach_absolute_time());

	tv->tv_sec = ns / 1000000000ULL;
	tv->tv_usec = (ns % 1000000000ULL) / 1000;
#elif defined(CLOCK_MONOTONIC)
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / 1000;
	} else {
		X_GETTIMEOFDAY(tv);
	}
#else
	X_GETTIMEOFDAY(tv);
#endif
	if (tv->tv_sec == 0 && tv->tv_usec == 0)
		tv->tv_usec = 1;
}

/* is t1 after t2 ? */
#define IS_AFTER(t1, t2)	(((t1).tv_sec > (t2).tv_sec) || \
    (((t1).tv_sec == (t2).tv_sec) \
    && ((t1).tv_usec > (t2).tv_usec)))

#define IS_ZERO(tv) (tv.tv_sec == 0 && tv.tv_usec == 0)

#define SET_ZERO(tv) tv.tv_sec = 0, tv.tv_usec = 0

static void addmillisecs(struct timeval *tv, int milliseconds)
{
	tv->tv_usec += milliseconds * 1000;

	tv->tv_sec += tv->tv_usec / 1000000;
	tv->tv_usec = tv->tv_usec % 1000000;
}

static void enqueueTimerHandler(TimerHandler * handler)
{
	TimerHandler *tmp;

	/* insert callback in queue, sorted by time left */
	if (!timerHandler || !IS_AFTER(handler->when, timerHandler->when)) {
		/* first in the queue */
		handler->next = timerHandler;
		timerHandler = handler;
	} else {
		tmp = timerHandler;
		while (tmp->next && IS_AFTER(handler->when, tmp->next->when)) {
			tmp = tmp->next;
		}
		handler->next = tmp->next;
		tmp->next = handler;
	}
}

static void delayUntilNextTimerEvent(struct timeval *delay)
{
	struct timeval now;
	TimerHandler *handler;

	handler = timerHandler;
	while (handler && IS_ZERO(handler->when))
		handler = handler->next;

	if (!handler) {
		/* The return value of this function is only valid if there _are_
		   timers active. */
		delay->tv_sec = 0;
		delay->tv_usec = 0;
		return;
	}

	rightNow(&now);
	if (IS_AFTER(now, handler->when)) {
		delay->tv_sec = 0;
		delay->tv_usec = 0;
	} else {
		delay->tv_sec = handler->when.tv_sec - now.tv_sec;
		delay->tv_usec = handler->when.tv_usec - now.tv_usec;
		if (delay->tv_usec < 0) {
			delay->tv_usec += 1000000;
			delay->tv_sec--;
		}
	}
}

/*
 * The next wake-up window: *soft is the earliest deadline, *hard the
 * earliest of (deadline + leeway) over all pending timers, so that no timer
 * is held past its own leeway. Returns 0 if no timer is pending (timers
 * whose callback is running have a zero `when` and do not count).
 */
static int nextTimerWindow(struct timeval *soft, struct timeval *hard)
{
	TimerHandler *h = timerHandler;

	while (h && IS_ZERO(h->when))
		h = h->next;
	if (!h)
		return 0;

	*soft = h->when;
	*hard = h->when;
	addmillisecs(hard, h->leeway);
	for (h = h->next; h; h = h->next) {
		struct timeval t;

		if (IS_ZERO(h->when))
			continue;
		t = h->when;
		addmillisecs(&t, h->leeway);
		if (IS_AFTER(*hard, t))
			*hard = t;
	}
	return 1;
}

/* Default leeway: 10% of the interval, at most 50 ms. */
static int defaultLeeway(int milliseconds)
{
	int l = milliseconds / 10;

	return l > 50 ? 50 : (l < 0 ? 0 : l);
}

WMHandlerID WMAddTimerHandler(int milliseconds, WMCallback * callback, void *cdata)
{
	return WMAddTimerHandlerWithLeeway(milliseconds, defaultLeeway(milliseconds), callback, cdata);
}

WMHandlerID WMAddTimerHandlerWithLeeway(int milliseconds, int leewayMs, WMCallback * callback, void *cdata)
{
	TimerHandler *handler;

	handler = malloc(sizeof(TimerHandler));
	if (!handler)
		return NULL;

	rightNow(&handler->when);
	addmillisecs(&handler->when, milliseconds);
	handler->callback = callback;
	handler->clientData = cdata;
	handler->nextDelay = 0;
	handler->leeway = leewayMs < 0 ? 0 : leewayMs;

	enqueueTimerHandler(handler);

	return handler;
}

WMHandlerID WMAddPersistentTimerHandler(int milliseconds, WMCallback * callback, void *cdata)
{
	return WMAddPersistentTimerHandlerWithLeeway(milliseconds, defaultLeeway(milliseconds), callback, cdata);
}

WMHandlerID WMAddPersistentTimerHandlerWithLeeway(int milliseconds, int leewayMs, WMCallback * callback, void *cdata)
{
	TimerHandler *handler = WMAddTimerHandlerWithLeeway(milliseconds, leewayMs, callback, cdata);

	if (handler != NULL)
		handler->nextDelay = milliseconds;

	return handler;
}

void WMDeleteTimerWithClientData(void *cdata)
{
	TimerHandler *handler, *tmp;

	if (!cdata || !timerHandler)
		return;

	tmp = timerHandler;
	if (tmp->clientData == cdata) {
		tmp->nextDelay = 0;
		if (!IS_ZERO(tmp->when)) {
			timerHandler = tmp->next;
			wfree(tmp);
		}
	} else {
		while (tmp->next) {
			if (tmp->next->clientData == cdata) {
				handler = tmp->next;
				handler->nextDelay = 0;
				if (IS_ZERO(handler->when))
					break;
				tmp->next = handler->next;
				wfree(handler);
				break;
			}
			tmp = tmp->next;
		}
	}
}

void WMDeleteTimerHandler(WMHandlerID handlerID)
{
	TimerHandler *tmp, *handler = (TimerHandler *) handlerID;

	if (!handler || !timerHandler)
		return;

	tmp = timerHandler;

	handler->nextDelay = 0;

	if (IS_ZERO(handler->when))
		return;

	if (tmp == handler) {
		timerHandler = handler->next;
		wfree(handler);
	} else {
		while (tmp->next) {
			if (tmp->next == handler) {
				tmp->next = handler->next;
				wfree(handler);
				break;
			}
			tmp = tmp->next;
		}
	}
}

WMHandlerID WMAddIdleHandler(WMCallback * callback, void *cdata)
{
	IdleHandler *handler;

	handler = malloc(sizeof(IdleHandler));
	if (!handler)
		return NULL;

	handler->callback = callback;
	handler->clientData = cdata;
	/* add handler at end of queue */
	if (!idleHandler) {
		idleHandler = WMCreateArrayWithDestructor(16, wfree);
	}
	WMAddToArray(idleHandler, handler);

	return handler;
}

void WMDeleteIdleHandler(WMHandlerID handlerID)
{
	IdleHandler *handler = (IdleHandler *) handlerID;

	if (!handler || !idleHandler)
		return;

	WMRemoveFromArray(idleHandler, handler);
}

#ifdef WM_USE_KQUEUE
/*
 * kqueue backend for the input wait (DAR-431)
 * -------------------------------------------
 * One kqueue, owned by this file, is the single place the process blocks.
 * It replaces the per-wakeup FD_ZERO/FD_SET rebuild and select() of
 * handleInputEventsSelect() below, which stays as the runtime fallback
 * (kqueue() failing, a registration failing for a reason other than a bad
 * fd, or WM_EVENT_BACKEND=select in the environment).
 *
 * Semantics kept from the select path:
 *  - Level-triggered. Registrations never use EV_CLEAR, so kevent()
 *    reports an fd for as long as it is ready, exactly like select().
 *    For EVFILT_READ on a socket the filter re-evaluates the socket
 *    buffer on every scan (xnu bsd/kern/uipc_socket.c filt_soread). This
 *    is what makes Xlib safe: waitForEvent() (wevent.c) only gets here
 *    after XPending()/XCheckMaskEvent() found nothing in Xlib's queue, and
 *    if the X socket holds unread bytes kevent() returns at once.
 *  - EINTR: kevent() never restarts after a signal, even with SA_RESTART
 *    (xnu bsd/kern/kern_event.c kevent_internal: "don't restart after
 *    signals...", ERESTART -> EINTR). wmaker's handlers use SA_RESTART
 *    (src/startup.c:527,546), so this matches select(), which also
 *    returns EINTR; the loop returns False and WMNextEvent goes round.
 *  - Timeout comes from the same timer queue as before.
 *  - A handler deleted by an earlier callback in the same batch does not
 *    fire: dispatch works on a copy of the handler array and checks
 *    membership, as the select path does.
 *
 * Registration model: per fd, the wanted filters are the union of all
 * handlers on that fd (WIReadMask->EVFILT_READ, WIWriteMask->EVFILT_WRITE,
 * WIExceptMask->EVFILT_EXCEPT+NOTE_OOB, event.h:433-434) plus the read
 * filter for the extra fd passed by wevent.c (ConnectionNumber(dpy)). The
 * kernel drops a knote when its fd is closed (kern_descrip.c:3414
 * knote_fdclose), so a handler whose fd was closed without being deleted
 * just never fires; select() would have returned EBADF forever.
 *
 * Fork: xnu marks a kqueue fd UF_EXCLOSE|UF_FORKCLOSE (kern_event.c:3025),
 * so a forked child has no kqueue. A pthread_atfork child hook forgets the
 * fd; the next wait creates a new kqueue and re-registers everything.
 *
 * Extension point for other filters (DAR-432 timers, DAR-433 vnode
 * watches, child exits): W_KQueueAddFilter()/W_KQueueDeleteFilter().
 * Such a filter's udata is its KQFilter; the callback runs from the same
 * wait, on the same thread, in the same batch as input handlers. They
 * need no change to the wait itself. They are lost if the backend falls
 * back to select, so each user keeps its own timer/poll fallback and
 * checks W_KQueueAddFilter() != NULL. The timeout passed to kevent()
 * is still computed from the timer queue, so DAR-432 can either keep
 * that and only swap the clock, or arm an EVFILT_TIMER here and pass a
 * NULL timeout.
 */
#define KQ_MAXEVENTS 32

/*
 * kevent64() as a raw syscall. All of this file's kqueue traffic uses it,
 * for two reasons:
 *  - EVFILT_TIMER's leeway travels in ext[1] (DAR-432), which legacy
 *    kevent() cannot carry (it zero-fills the extension fields);
 *  - a kqueue is either "legacy32" (kevent()) or not (kevent64()/
 *    kevent_qos()) from its first use on, and the other flavour is refused
 *    with EINVAL (bsd/kern/kern_event.c:6873-6877, kevent_get_kqfile), so
 *    the wait, the registrations and the timer must all use one of them.
 * This port's libsystem_kernel exports neither kevent64 nor kevent_qos,
 * so the syscall (bsd/kern/syscalls.master:560, number SYS_kevent64) is
 * made directly: arm64 Darwin takes the number in x16 with `svc #0x80`;
 * carry set means failure, errno in x0 (bsd/dev/arm/systemcalls.c); on
 * success x0 is the number of events returned.
 * Returns that number, or -1 with errno set.
 */
static int kq64(int kq, const struct kevent64_s *change, int nchanges,
		struct kevent64_s *events, int nevents, const struct timespec *timeout)
{
	register long x0 __asm__("x0") = kq;
	register long x1 __asm__("x1") = (long)change;
	register long x2 __asm__("x2") = nchanges;
	register long x3 __asm__("x3") = (long)events;
	register long x4 __asm__("x4") = nevents;
	register long x5 __asm__("x5") = 0;
	register long x6 __asm__("x6") = (long)timeout;
	register long x16 __asm__("x16") = SYS_kevent64;
	unsigned long failed;

	__asm__ volatile ("svc #0x80\n\tcset %0, cs"
			  : "=r" (failed), "+r" (x0), "+r" (x1)
			  : "r" (x2), "r" (x3), "r" (x4), "r" (x5), "r" (x6), "r" (x16)
			  : "memory", "cc");
	if (failed) {
		errno = (int)x0;
		return -1;
	}
	return (int)x0;
}

static void kevTo64(const struct kevent *k, struct kevent64_s *k64, void *udata)
{
	memset(k64, 0, sizeof *k64);
	k64->ident = k->ident;
	k64->filter = k->filter;
	k64->flags = k->flags;
	k64->fflags = k->fflags;
	k64->data = k->data;
	k64->udata = (uint64_t)(uintptr_t)udata;
}

static void kevFrom64(const struct kevent64_s *k64, struct kevent *k)
{
	memset(k, 0, sizeof *k);
	k->ident = (uintptr_t)k64->ident;
	k->filter = k64->filter;
	k->flags = k64->flags;
	k->fflags = k64->fflags;
	k->data = (intptr_t)k64->data;
	k->udata = (void *)(uintptr_t)k64->udata;
}

typedef struct KQFilter {
	struct kevent kev;
	W_KQueueProc *proc;
	void *clientData;
} KQFilter;

static int kq_fd = -1;
static int kq_disabled = 0;	/* 1: use the select path */
static int kq_xfd = -1;		/* the extra fd (X connection) */
static int kq_atfork_done = 0;
static WMArray *kq_filters = NULL;
static char kq_fd_tag;		/* udata of every fd-input registration */
#ifndef WM_KQ_TIMER
# define KQ_IS_TIMER_TAG(p)	0
#else
# define KQ_IS_TIMER_TAG(p)	((void *)(p) == (void *)&kq_timer_tag)
static char kq_timer_tag;	/* udata of the timer-queue EVFILT_TIMER */
static int kq_timer_ok = 1;	/* 0: unsupported here, use the kevent() timeout */
static int kq_timer_errno;	/* errno of the registration that disabled it */
static int kq_timer_armed;	/* a knote for (kq_timer_deadline, kq_timer_leeway) exists */
static uint64_t kq_timer_deadline, kq_timer_leeway;
static unsigned long stat_timer_arms, stat_timer_wakes;
#endif

static void kq_disable(void)
{
	if (kq_fd >= 0)
		close(kq_fd);
	kq_fd = -1;
	kq_disabled = 1;
#ifdef WM_KQ_TIMER
	kq_timer_armed = 0;
#endif
}

static int kq_change(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags, void *udata)
{
	struct kevent64_s ev;

	EV_SET64(&ev, ident, filter, flags, fflags, 0, (uint64_t)(uintptr_t)udata, 0, 0);
	return kq64(kq_fd, &ev, 1, NULL, 0, NULL);
}

/*
 * Make the registrations for fd match the handlers (plus xfd). Filters no
 * longer wanted are only deleted when 'removing' (a handler was deleted),
 * so adding costs one syscall per wanted filter and nothing else.
 */
static void kq_sync_fd(int fd, int removing)
{
	int want = 0, i, n = inputHandler ? WMGetArrayItemCount(inputHandler) : 0;

	if (fd < 0)
		return;
	for (i = 0; i < n; i++) {
		InputHandler *h = WMGetFromArray(inputHandler, i);
		if (h->fd == fd)
			want |= h->mask;
	}
	if (fd == kq_xfd)
		want |= WIReadMask;

	if (want & WIReadMask) {
		if (kq_change(fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, &kq_fd_tag) < 0 && errno != EBADF)
			kq_disable();
	} else if (removing) {
		kq_change(fd, EVFILT_READ, EV_DELETE, 0, NULL);	/* ENOENT is fine */
	}
	if (kq_fd < 0)
		return;
	if (want & WIWriteMask) {
		if (kq_change(fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, &kq_fd_tag) < 0 && errno != EBADF)
			kq_disable();
	} else if (removing) {
		kq_change(fd, EVFILT_WRITE, EV_DELETE, 0, NULL);
	}
	if (kq_fd < 0)
		return;
	if (want & WIExceptMask) {
		if (kq_change(fd, EVFILT_EXCEPT, EV_ADD | EV_ENABLE, NOTE_OOB, &kq_fd_tag) < 0 && errno != EBADF)
			kq_disable();
	} else if (removing) {
		kq_change(fd, EVFILT_EXCEPT, EV_DELETE, 0, NULL);
	}
}

static void kq_forked_child(void)
{
	/* the kernel did not copy the kqueue into the child (see above) */
	kq_fd = -1;
#ifdef WM_KQ_TIMER
	kq_timer_armed = 0;
#endif
}

/* Create the kqueue on first use. Returns 1 if the kqueue backend is live. */
static int kq_init(void)
{
	int i, n;
	const char *env;

	if (kq_fd >= 0)
		return 1;
	if (kq_disabled)
		return 0;

	env = getenv("WM_EVENT_BACKEND");
	if (env && strcmp(env, "select") == 0) {
		kq_disabled = 1;
		return 0;
	}

	kq_fd = kqueue();
	if (kq_fd < 0) {
		kq_disabled = 1;
		return 0;
	}
	fcntl(kq_fd, F_SETFD, FD_CLOEXEC);
#ifdef WM_KQ_TIMER
	kq_timer_armed = 0;
#endif

	if (!kq_atfork_done) {
		kq_atfork_done = 1;
		pthread_atfork(NULL, NULL, kq_forked_child);
	}

	/* first use, or first use after fork: register everything known */
	n = inputHandler ? WMGetArrayItemCount(inputHandler) : 0;
	for (i = 0; i < n && kq_fd >= 0; i++)
		kq_sync_fd(((InputHandler *) WMGetFromArray(inputHandler, i))->fd, 0);
	if (kq_fd >= 0 && kq_xfd >= 0)
		kq_sync_fd(kq_xfd, 0);
	n = kq_filters ? WMGetArrayItemCount(kq_filters) : 0;
	for (i = 0; i < n && kq_fd >= 0; i++) {
		KQFilter *f = WMGetFromArray(kq_filters, i);
		/* a failure here is that filter's owner's problem: it polls as before */
		(void) kq_change(f->kev.ident, f->kev.filter, f->kev.flags | EV_ADD, f->kev.fflags, f);
	}

	return kq_fd >= 0;
}

W_KQueueID W_KQueueAddFilter(const struct kevent *kev, W_KQueueProc *proc, void *clientData)
{
	KQFilter *f;
	struct kevent64_s ev;

	if (!kq_init())
		return NULL;

	f = wmalloc(sizeof(KQFilter));
	f->kev = *kev;
	f->proc = proc;
	f->clientData = clientData;

	kevTo64(kev, &ev, f);
	ev.flags |= EV_ADD;
	if (kq64(kq_fd, &ev, 1, NULL, 0, NULL) < 0) {
		wfree(f);
		return NULL;
	}
	f->kev.udata = f;

	if (!kq_filters)
		kq_filters = WMCreateArrayWithDestructor(8, wfree);
	WMAddToArray(kq_filters, f);

	return f;
}

void W_KQueueDeleteFilter(W_KQueueID id)
{
	KQFilter *f = id;

	if (!f || !kq_filters || WMGetFirstInArray(kq_filters, f) == WANotFound)
		return;
	if (kq_fd >= 0)
		kq_change(f->kev.ident, f->kev.filter, EV_DELETE, 0, NULL);
	WMRemoveFromArray(kq_filters, f);
}

#ifdef WM_KQ_TIMER
/*
 * The timer queue on the kernel's timer (DAR-432)
 * -----------------------------------------------
 * One EVFILT_TIMER knote stands for the whole queue: it is armed for the
 * queue's next wake-up window (nextTimerWindow) and kevent() then blocks
 * with no timeout. Why, and what it needs:
 *  - NOTE_MACHTIME|NOTE_ABSOLUTE: `data` is a deadline in mach_absolute_time
 *    units (sys/event.h:568-576; filt_timervalidate, kern_event.c:1312-1313,
 *    1368-1369), the clock rightNow() uses. NOTE_ABSOLUTE makes it a one-shot
 *    (filt_timerattach, kern_event.c:1631-1633), so it is re-armed after each
 *    delivery. A deadline in the past fires at once (filt_timer_is_ready).
 *  - NOTE_LEEWAY: ext[1] is the leeway in the same units (kern_event.c:
 *    1348-1363). filt_timerarm passes it to thread_call_enter_delayed_with_
 *    leeway (kern_event.c:1574), which uses max(leeway, the default slop of
 *    the thread's QoS tier) as the coalescing slop and sets the hard
 *    deadline to deadline + slop (thread_call.c:1223-1232). The kernel then
 *    wakes us at the hard deadline unless some other timer wakes the CPU
 *    first; that is what lets idle wake-ups coalesce. Leeway here is
 *    (earliest hard deadline - earliest soft deadline), so no timer is
 *    delayed past its own leeway.
 *  - The kevent() TIMEOUT cannot do this: kqueue_scan waits with
 *    TIMEOUT_NO_LEEWAY (kern_event.c:7510-7512). That timeout stays as the
 *    fallback if the knote cannot be registered.
 * Changing the registered deadline/leeway is a touch of the same knote
 * (filt_timertouch, kern_event.c:1665-1698), so re-arming needs no delete.
 *
 * The leeway field is ext[1], which only kevent64() can pass here (see
 * kq64() above).
 */
static uint64_t timevalToAbs(const struct timeval *tv)
{
	uint64_t ns = (uint64_t)tv->tv_sec * 1000000000ULL + (uint64_t)tv->tv_usec * 1000ULL;

	monoInit();
	return mulDiv(ns, mono_tb.denom, mono_tb.numer);
}

/*
 * Arm the queue's knote for the next window. Returns 1 if it is armed (the
 * caller blocks with no timeout), 0 if not (no pending timer, or the kernel
 * refused: use the timeout). Only issues a syscall when the window changed.
 */
static int kq_timer_arm(void)
{
	struct timeval soft, hard;
	struct kevent64_s ev;
	uint64_t deadline, hardAbs, leeway;

	if (!kq_timer_ok || !nextTimerWindow(&soft, &hard))
		return 0;

	deadline = timevalToAbs(&soft);
	hardAbs = timevalToAbs(&hard);
	leeway = hardAbs > deadline ? hardAbs - deadline : 0;
	if (kq_timer_armed && deadline == kq_timer_deadline && leeway == kq_timer_leeway)
		return 1;

	memset(&ev, 0, sizeof ev);
	ev.ident = 1;
	ev.filter = EVFILT_TIMER;
	ev.flags = EV_ADD | EV_ENABLE;
	ev.fflags = NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_LEEWAY;
	ev.data = (int64_t)deadline;
	ev.udata = (uint64_t)(uintptr_t)&kq_timer_tag;
	ev.ext[1] = leeway;
	if (kq64(kq_fd, &ev, 1, NULL, 0, NULL) < 0) {
		kq_timer_errno = errno;
		kq_timer_ok = 0;
		kq_timer_armed = 0;
		return 0;
	}
	kq_timer_armed = 1;
	kq_timer_deadline = deadline;
	kq_timer_leeway = leeway;
	stat_timer_arms++;
	return 1;
}

/* No timer is pending any more: stop a knote that would only cause a spurious wake-up. */
static void kq_timer_disarm(void)
{
	if (kq_timer_armed) {
		kq_change(1, EVFILT_TIMER, EV_DELETE, 0, NULL);
		kq_timer_armed = 0;
	}
}
#endif /* WM_KQ_TIMER */

/* Returns -1 if the backend is unavailable (caller uses select). */
static int kq_handleInputEvents(Bool waitForInput, int inputfd)
{
	struct kevent64_s evs[KQ_MAXEVENTS];
	struct timespec ts, *tsp;
	int nfds, nfilters, n, nevents, i, j;

	if (!kq_init())
		return -1;

	nfds = inputHandler ? WMGetArrayItemCount(inputHandler) : 0;
	nfilters = kq_filters ? WMGetArrayItemCount(kq_filters) : 0;

	if (inputfd < 0 && nfds == 0 && nfilters == 0) {
		W_FlushASAPNotificationQueue();
		return False;
	}

	if (inputfd != kq_xfd) {
		int old = kq_xfd;
		kq_xfd = inputfd;
		kq_sync_fd(old, 1);
		kq_sync_fd(inputfd, 0);
		if (kq_fd < 0)
			return -1;	/* a registration failed: select from now on */
	}

	if (!waitForInput) {
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
		tsp = &ts;
	} else if (timerPending()) {
#ifdef WM_KQ_TIMER
		if (kq_timer_arm()) {
			tsp = NULL;	/* the knote is the timeout */
		} else
#endif
		{
			struct timeval tv;
			delayUntilNextTimerEvent(&tv);
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = tv.tv_usec * 1000;
			tsp = &ts;
		}
	} else {
#ifdef WM_KQ_TIMER
		kq_timer_disarm();
#endif
		tsp = NULL;
	}

	n = kq64(kq_fd, NULL, 0, evs, KQ_MAXEVENTS, tsp);
	nevents = n;
#ifdef WM_KQ_TIMER
	/* the queue's knote is one-shot: it is gone once delivered. It is not input. */
	for (i = 0; i < n; i++) {
		if (evs[i].udata == (uint64_t)(uintptr_t)&kq_timer_tag) {
			kq_timer_armed = 0;
			stat_timer_wakes++;
			nevents--;
		}
	}
#endif

	if (n > 0) {
		/* input handlers first, through a copy, as the select path does */
		if (nfds > 0) {
			WMArray *handlerCopy = WMDuplicateArray(inputHandler);

			for (i = 0; i < nfds; i++) {
				InputHandler *handler = WMGetFromArray(handlerCopy, i);
				int mask = 0;

				if (WMGetFirstInArray(inputHandler, handler) == WANotFound)
					continue;

				for (j = 0; j < n; j++) {
					if (evs[j].udata != (uint64_t)(uintptr_t)&kq_fd_tag || (int)evs[j].ident != handler->fd)
						continue;
					if (evs[j].filter == EVFILT_READ && (handler->mask & WIReadMask))
						mask |= WIReadMask;
					else if (evs[j].filter == EVFILT_WRITE && (handler->mask & WIWriteMask))
						mask |= WIWriteMask;
					else if (evs[j].filter == EVFILT_EXCEPT && (handler->mask & WIExceptMask))
						mask |= WIExceptMask;
				}

				if (mask != 0 && handler->callback)
					(*handler->callback) (handler->fd, mask, handler->clientData);
			}
			WMFreeArray(handlerCopy);
		}

		/* then other filters; one may have been deleted by an earlier callback */
		for (j = 0; j < n; j++) {
			KQFilter *f = (KQFilter *)(uintptr_t)evs[j].udata;

			if (f == (KQFilter *) &kq_fd_tag || KQ_IS_TIMER_TAG(f) || !kq_filters ||
			    WMGetFirstInArray(kq_filters, f) == WANotFound)
				continue;
			if (f->proc) {
				struct kevent ev;

				kevFrom64(&evs[j], &ev);
				(*f->proc) (&ev, f->clientData);
			}
			if ((f->kev.flags & EV_ONESHOT) && kq_filters &&
			    WMGetFirstInArray(kq_filters, f) != WANotFound)
				WMRemoveFromArray(kq_filters, f);
		}
	}

	W_FlushASAPNotificationQueue();

	return (nevents > 0);
}
#else /* !WM_USE_KQUEUE */

W_KQueueID W_KQueueAddFilter(const struct kevent *kev, W_KQueueProc *proc, void *clientData)
{
	return NULL;
}

void W_KQueueDeleteFilter(W_KQueueID id)
{
}
#endif /* WM_USE_KQUEUE */

WMHandlerID WMAddInputHandler(int fd, int condition, WMInputProc * proc, void *clientData)
{
	InputHandler *handler;

	handler = wmalloc(sizeof(InputHandler));

	handler->fd = fd;
	handler->mask = condition;
	handler->callback = proc;
	handler->clientData = clientData;

	if (!inputHandler)
		inputHandler = WMCreateArrayWithDestructor(16, wfree);
	WMAddToArray(inputHandler, handler);

#ifdef WM_USE_KQUEUE
	/* if the kqueue already exists register now, else kq_init() does it */
	if (kq_fd >= 0)
		kq_sync_fd(fd, 0);
#endif

	return handler;
}

void WMDeleteInputHandler(WMHandlerID handlerID)
{
	InputHandler *handler = (InputHandler *) handlerID;

	if (!handler || !inputHandler)
		return;

#ifdef WM_USE_KQUEUE
	{
		/* the array destructor frees the handler, so keep the fd */
		int fd = handler->fd;
		WMRemoveFromArray(inputHandler, handler);
		if (kq_fd >= 0)
			kq_sync_fd(fd, 1);
	}
#else
	WMRemoveFromArray(inputHandler, handler);
#endif
}

Bool W_CheckIdleHandlers(void)
{
	IdleHandler *handler;
	WMArray *handlerCopy;
	WMArrayIterator iter;

	if (!idleHandler || WMGetArrayItemCount(idleHandler) == 0) {
		W_FlushIdleNotificationQueue();
		/* make sure an observer in queue didn't added an idle handler */
		return (idleHandler != NULL && WMGetArrayItemCount(idleHandler) > 0);
	}

	handlerCopy = WMDuplicateArray(idleHandler);

	WM_ITERATE_ARRAY(handlerCopy, handler, iter) {
		/* check if the handler still exist or was removed by a callback */
		if (WMGetFirstInArray(idleHandler, handler) == WANotFound)
			continue;

		(*handler->callback) (handler->clientData);
		WMDeleteIdleHandler(handler);
	}

	WMFreeArray(handlerCopy);

	W_FlushIdleNotificationQueue();

	/* this is not necesarrily False, because one handler can re-add itself */
	return (WMGetArrayItemCount(idleHandler) > 0);
}

/*
 * WM_EVENT_STATS also counts how often each timer callback runs (first 16
 * distinct callbacks), printed as image+offset so they can be looked up
 * with nm in the unstripped binary: which timers are alive on an idle
 * desktop (DAR-432).
 */
static int stat_enabled = -1;	/* -1: not read yet; WM_EVENT_STATS */
static struct { void *cb; unsigned long n; } timer_fires[16];

static void countTimerFire(void *cb)
{
	int i;

	for (i = 0; i < 16; i++) {
		if (timer_fires[i].cb == cb || timer_fires[i].cb == NULL) {
			timer_fires[i].cb = cb;
			timer_fires[i].n++;
			return;
		}
	}
}

void W_CheckTimerHandlers(void)
{
	TimerHandler *handler;
	struct timeval now;

	if (!timerHandler) {
		W_FlushASAPNotificationQueue();
		return;
	}

	rightNow(&now);

	handler = timerHandler;
	while (handler && IS_AFTER(now, handler->when)) {
		if (!IS_ZERO(handler->when)) {
			SET_ZERO(handler->when);
			if (stat_enabled > 0)
				countTimerFire((void *)handler->callback);
			(*handler->callback) (handler->clientData);
		}
		handler = handler->next;
	}

	while (timerHandler && IS_ZERO(timerHandler->when)) {
		handler = timerHandler;
		timerHandler = timerHandler->next;

		if (handler->nextDelay > 0) {
			handler->when = now;
			addmillisecs(&handler->when, handler->nextDelay);
			enqueueTimerHandler(handler);
		} else {
			wfree(handler);
		}
	}

	W_FlushASAPNotificationQueue();
}

/*
 * This functions will handle input events on all registered file descriptors.
 * Input:
 *    - waitForInput - True if we want the function to wait until an event
 *                     appears on a file descriptor we watch, False if we
 *                     want the function to immediately return if there is
 *                     no data available on the file descriptors we watch.
 *    - inputfd      - Extra input file descriptor to watch for input.
 *                     This is only used when called from wevent.c to watch
 *                     on ConnectionNumber(dpy) to avoid blocking of X events
 *                     if we wait for input from other file handlers.
 * Output:
 *    if waitForInput is False, the function will return False if there are no
 *                     input handlers registered, or if there is no data
 *                     available on the registered ones, and will return True
 *                     if there is at least one input handler that has data
 *                     available.
 *    if waitForInput is True, the function will return False if there are no
 *                     input handlers registered, else it will block until an
 *                     event appears on one of the file descriptors it watches
 *                     and then it will return True.
 *
 * If the retured value is True, the input handlers for the corresponding file
 * descriptors are also called.
 *
 * Parametersshould be passed like this:
 * - from wevent.c:
 *   waitForInput - apropriate value passed by the function who called us
 *   inputfd = ConnectionNumber(dpy)
 * - from wutil.c:
 *   waitForInput - apropriate value passed by the function who called us
 *   inputfd = -1
 *
 */
static Bool handleInputEventsSelect(Bool waitForInput, int inputfd)
{
#if defined(HAVE_POLL) && defined(HAVE_POLL_H) && !defined(HAVE_SELECT)
	struct poll fd *fds;
	InputHandler *handler;
	int count, timeout, nfds, i, extrafd;

	extrafd = (inputfd < 0) ? 0 : 1;

	if (inputHandler)
		nfds = WMGetArrayItemCount(inputHandler);
	else
		nfds = 0;

	if (!extrafd && nfds == 0) {
		W_FlushASAPNotificationQueue();
		return False;
	}

	fds = wmalloc((nfds + extrafd) * sizeof(struct pollfd));
	if (extrafd) {
		/* put this to the end of array to avoid using ranges from 1 to nfds+1 */
		fds[nfds].fd = inputfd;
		fds[nfds].events = POLLIN;
	}

	/* use WM_ITERATE_ARRAY() here */
	for (i = 0; i < nfds; i++) {
		handler = WMGetFromArray(inputHandler, i);
		fds[i].fd = handler->fd;
		fds[i].events = 0;
		if (handler->mask & WIReadMask)
			fds[i].events |= POLLIN;

		if (handler->mask & WIWriteMask)
			fds[i].events |= POLLOUT;

#if 0				/* FIXME */
		if (handler->mask & WIExceptMask)
			FD_SET(handler->fd, &eset);
#endif
	}

	/*
	 * Setup the timeout to the estimated time until the
	 * next timer expires.
	 */
	if (!waitForInput) {
		timeout = 0;
	} else if (timerPending()) {
		struct timeval tv;
		delayUntilNextTimerEvent(&tv);
		timeout = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	} else {
		timeout = -1;
	}

	count = poll(fds, nfds + extrafd, timeout);

	if (count > 0 && nfds > 0) {
		WMArray *handlerCopy = WMDuplicateArray(inputHandler);
		int mask;

		/* use WM_ITERATE_ARRAY() here */
		for (i = 0; i < nfds; i++) {
			handler = WMGetFromArray(handlerCopy, i);
			/* check if the handler still exist or was removed by a callback */
			if (WMGetFirstInArray(inputHandler, handler) == WANotFound)
				continue;

			mask = 0;

			if ((handler->mask & WIReadMask) &&
			    (fds[i].revents & (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI)))
				mask |= WIReadMask;

			if ((handler->mask & WIWriteMask) && (fds[i].revents & (POLLOUT | POLLWRBAND)))
				mask |= WIWriteMask;

			if ((handler->mask & WIExceptMask) && (fds[i].revents & (POLLHUP | POLLNVAL | POLLERR)))
				mask |= WIExceptMask;

			if (mask != 0 && handler->callback) {
				(*handler->callback) (handler->fd, mask, handler->clientData);
			}
		}

		WMFreeArray(handlerCopy);
	}

	wfree(fds);

	W_FlushASAPNotificationQueue();

	return (count > 0);
#else
#ifdef HAVE_SELECT
	struct timeval timeout;
	struct timeval *timeoutPtr;
	fd_set rset, wset, eset;
	int maxfd, nfds, i;
	int count;
	InputHandler *handler;

	if (inputHandler)
		nfds = WMGetArrayItemCount(inputHandler);
	else
		nfds = 0;

	if (inputfd < 0 && nfds == 0) {
		W_FlushASAPNotificationQueue();
		return False;
	}

	FD_ZERO(&rset);
	FD_ZERO(&wset);
	FD_ZERO(&eset);

	if (inputfd < 0) {
		maxfd = 0;
	} else {
		FD_SET(inputfd, &rset);
		maxfd = inputfd;
	}

	/* use WM_ITERATE_ARRAY() here */
	for (i = 0; i < nfds; i++) {
		handler = WMGetFromArray(inputHandler, i);
		if (handler->mask & WIReadMask)
			FD_SET(handler->fd, &rset);

		if (handler->mask & WIWriteMask)
			FD_SET(handler->fd, &wset);

		if (handler->mask & WIExceptMask)
			FD_SET(handler->fd, &eset);

		if (maxfd < handler->fd)
			maxfd = handler->fd;
	}

	/*
	 * Setup the timeout to the estimated time until the
	 * next timer expires.
	 */
	if (!waitForInput) {
		SET_ZERO(timeout);
		timeoutPtr = &timeout;
	} else if (timerPending()) {
		delayUntilNextTimerEvent(&timeout);
		timeoutPtr = &timeout;
	} else {
		timeoutPtr = (struct timeval *)0;
	}

	count = select(1 + maxfd, &rset, &wset, &eset, timeoutPtr);

	if (count > 0 && nfds > 0) {
		WMArray *handlerCopy = WMDuplicateArray(inputHandler);
		int mask;

		/* use WM_ITERATE_ARRAY() here */
		for (i = 0; i < nfds; i++) {
			handler = WMGetFromArray(handlerCopy, i);
			/* check if the handler still exist or was removed by a callback */
			if (WMGetFirstInArray(inputHandler, handler) == WANotFound)
				continue;

			mask = 0;

			if ((handler->mask & WIReadMask) && FD_ISSET(handler->fd, &rset))
				mask |= WIReadMask;

			if ((handler->mask & WIWriteMask) && FD_ISSET(handler->fd, &wset))
				mask |= WIWriteMask;

			if ((handler->mask & WIExceptMask) && FD_ISSET(handler->fd, &eset))
				mask |= WIExceptMask;

			if (mask != 0 && handler->callback) {
				(*handler->callback) (handler->fd, mask, handler->clientData);
			}
		}

		WMFreeArray(handlerCopy);
	}

	W_FlushASAPNotificationQueue();

	return (count > 0);
#else				/* not HAVE_SELECT, not HAVE_POLL */
# error   Neither select nor poll. You lose.
#endif				/* HAVE_SELECT */
#endif				/* HAVE_POLL */
}

/*
 * WM_EVENT_STATS=N: count blocking waits and print the totals to stderr at
 * exit, and at most every N seconds (N > 1) from inside the wait, so idle
 * wakeups can be compared between the backends
 * (WM_EVENT_BACKEND=select|kqueue). Two integer increments per wait; the
 * time() call is made only when N > 1.
 */
static unsigned long stat_waits, stat_woke;	/* blocking waits, returns with input */
static int stat_interval;
static time_t stat_last;
static const char *stat_backend = "select";

static void printStats(void)
{
	int i;

	for (i = 0; i < 16 && timer_fires[i].cb; i++) {
#ifdef __APPLE__
		Dl_info di;

		if (dladdr(timer_fires[i].cb, &di) && di.dli_fbase)
			fprintf(stderr, "WUtil timer callback %s+0x%lx (%s) fired %lu\n",
				di.dli_fname, (unsigned long)((char *)timer_fires[i].cb - (char *)di.dli_fbase),
				di.dli_sname ? di.dli_sname : "?", timer_fires[i].n);
		else
#endif
			fprintf(stderr, "WUtil timer callback %p fired %lu\n", timer_fires[i].cb, timer_fires[i].n);
	}
#ifdef WM_KQ_TIMER
	fprintf(stderr, "WUtil event stats: t=%ld backend=%s blocking_waits=%lu with_input=%lu "
		"ktimer=%s errno=%d timer_arms=%lu timer_wakes=%lu\n",
		(long)time(NULL), stat_backend, stat_waits, stat_woke,
		kq_timer_ok ? "on" : "off", kq_timer_errno, stat_timer_arms, stat_timer_wakes);
#else
	fprintf(stderr, "WUtil event stats: t=%ld backend=%s blocking_waits=%lu with_input=%lu\n",
		(long)time(NULL), stat_backend, stat_waits, stat_woke);
#endif
}

static void periodicStats(void)
{
	time_t t = time(NULL);

	if (stat_interval > 1 && t - stat_last >= stat_interval) {
		stat_last = t;
		printStats();
	}
}

Bool W_HandleInputEvents(Bool waitForInput, int inputfd)
{
	Bool r;

	if (stat_enabled < 0) {
		const char *e = getenv("WM_EVENT_STATS");

		stat_enabled = e != NULL;
		if (stat_enabled) {
			stat_interval = atoi(e);
			atexit(printStats);
		}
	}
#ifdef WM_USE_KQUEUE
	{
		int k = kq_handleInputEvents(waitForInput, inputfd);

		if (k >= 0) {
			if (stat_enabled) {
				stat_backend = "kqueue";
				if (waitForInput) {
					stat_waits++;
					stat_woke += k;
					periodicStats();
				}
			}
			return k;
		}
	}
#endif
	r = handleInputEventsSelect(waitForInput, inputfd);
	if (stat_enabled && waitForInput) {
		stat_backend = "select";
		stat_waits++;
		stat_woke += r;
		periodicStats();
	}
	return r;
}
