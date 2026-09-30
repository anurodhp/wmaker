
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

#ifdef WM_USE_KQUEUE
# include <sys/event.h>
# include <fcntl.h>
# include <errno.h>
# include <string.h>
# include <pthread.h>
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

static void rightNow(struct timeval *tv)
{
	X_GETTIMEOFDAY(tv);
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

WMHandlerID WMAddTimerHandler(int milliseconds, WMCallback * callback, void *cdata)
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

	enqueueTimerHandler(handler);

	return handler;
}

WMHandlerID WMAddPersistentTimerHandler(int milliseconds, WMCallback * callback, void *cdata)
{
	TimerHandler *handler = WMAddTimerHandler(milliseconds, callback, cdata);

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

static void kq_disable(void)
{
	if (kq_fd >= 0)
		close(kq_fd);
	kq_fd = -1;
	kq_disabled = 1;
}

static int kq_change(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags, void *udata)
{
	struct kevent ev;

	EV_SET(&ev, ident, filter, flags, fflags, 0, udata);
	return kevent(kq_fd, &ev, 1, NULL, 0, NULL);
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
	struct kevent ev;

	if (!kq_init())
		return NULL;

	f = wmalloc(sizeof(KQFilter));
	f->kev = *kev;
	f->proc = proc;
	f->clientData = clientData;

	ev = *kev;
	ev.flags |= EV_ADD;
	ev.udata = f;
	if (kevent(kq_fd, &ev, 1, NULL, 0, NULL) < 0) {
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

/* Returns -1 if the backend is unavailable (caller uses select). */
static int kq_handleInputEvents(Bool waitForInput, int inputfd)
{
	struct kevent evs[KQ_MAXEVENTS];
	struct timespec ts, *tsp;
	int nfds, nfilters, n, i, j;

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
		struct timeval tv;
		delayUntilNextTimerEvent(&tv);
		ts.tv_sec = tv.tv_sec;
		ts.tv_nsec = tv.tv_usec * 1000;
		tsp = &ts;
	} else {
		tsp = NULL;
	}

	n = kevent(kq_fd, NULL, 0, evs, KQ_MAXEVENTS, tsp);

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
					if (evs[j].udata != &kq_fd_tag || (int)evs[j].ident != handler->fd)
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
			KQFilter *f = evs[j].udata;

			if (f == (KQFilter *) &kq_fd_tag || !kq_filters ||
			    WMGetFirstInArray(kq_filters, f) == WANotFound)
				continue;
			if (f->proc)
				(*f->proc) (&evs[j], f->clientData);
			if ((f->kev.flags & EV_ONESHOT) && kq_filters &&
			    WMGetFirstInArray(kq_filters, f) != WANotFound)
				WMRemoveFromArray(kq_filters, f);
		}
	}

	W_FlushASAPNotificationQueue();

	return (n > 0);
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
static int stat_enabled = -1, stat_interval;
static time_t stat_last;
static const char *stat_backend = "select";

static void printStats(void)
{
	fprintf(stderr, "WUtil event stats: t=%ld backend=%s blocking_waits=%lu with_input=%lu\n",
		(long)time(NULL), stat_backend, stat_waits, stat_woke);
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
