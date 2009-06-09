/*****************************************************************************
 * pthread.c : pthread back-end for LibVLC
 *****************************************************************************
 * Copyright (C) 1999-2009 the VideoLAN team
 *
 * Authors: Jean-Marc Dressler <polux@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *          Clément Sténac
 *          Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

#include "libvlc.h"
#include <stdarg.h>
#include <assert.h>
#include <unistd.h> /* fsync() */
#include <signal.h>

#include <sched.h>
#ifdef __linux__
# include <sys/syscall.h> /* SYS_gettid */
#endif

#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#ifdef __APPLE__
# include <sys/time.h> /* gettimeofday in vlc_cond_timedwait */
#endif

/**
 * Print a backtrace to the standard error for debugging purpose.
 */
void vlc_trace (const char *fn, const char *file, unsigned line)
{
     fprintf (stderr, "at %s:%u in %s\n", file, line, fn);
     fflush (stderr); /* needed before switch to low-level I/O */
#ifdef HAVE_BACKTRACE
     void *stack[20];
     int len = backtrace (stack, sizeof (stack) / sizeof (stack[0]));
     backtrace_symbols_fd (stack, len, 2);
#endif
     fsync (2);
}

static inline unsigned long vlc_threadid (void)
{
#if defined (__linux__)
     /* glibc does not provide a call for this */
     return syscall (SYS_gettid);

#else
     union { pthread_t th; unsigned long int i; } v = { };
     v.th = pthread_self ();
     return v.i;

#endif
}

#ifndef NDEBUG
/*****************************************************************************
 * vlc_thread_fatal: Report an error from the threading layer
 *****************************************************************************
 * This is mostly meant for debugging.
 *****************************************************************************/
static void
vlc_thread_fatal (const char *action, int error,
                  const char *function, const char *file, unsigned line)
{
    fprintf (stderr, "LibVLC fatal error %s (%d) in thread %lu ",
             action, error, vlc_threadid ());
    vlc_trace (function, file, line);

    /* Sometimes strerror_r() crashes too, so make sure we print an error
     * message before we invoke it */
#ifdef __GLIBC__
    /* Avoid the strerror_r() prototype brain damage in glibc */
    errno = error;
    fprintf (stderr, " Error message: %m\n");
#else
    char buf[1000];
    const char *msg;

    switch (strerror_r (error, buf, sizeof (buf)))
    {
        case 0:
            msg = buf;
            break;
        case ERANGE: /* should never happen */
            msg = "unknwon (too big to display)";
            break;
        default:
            msg = "unknown (invalid error number)";
            break;
    }
    fprintf (stderr, " Error message: %s\n", msg);
#endif
    fflush (stderr);

    abort ();
}

# define VLC_THREAD_ASSERT( action ) \
    if (val) vlc_thread_fatal (action, val, __func__, __FILE__, __LINE__)
#else
# define VLC_THREAD_ASSERT( action ) ((void)val)
#endif

#if defined (__GLIBC__) && (__GLIBC_MINOR__ < 6)
/* This is not prototyped under glibc, though it exists. */
int pthread_mutexattr_setkind_np( pthread_mutexattr_t *attr, int kind );
#endif

/*****************************************************************************
 * vlc_mutex_init: initialize a mutex
 *****************************************************************************/
void vlc_mutex_init( vlc_mutex_t *p_mutex )
{
    pthread_mutexattr_t attr;

    if( pthread_mutexattr_init( &attr ) )
        abort();
#ifndef NDEBUG
    /* Create error-checking mutex to detect problems more easily. */
# if defined (__GLIBC__) && (__GLIBC_MINOR__ < 6)
    pthread_mutexattr_setkind_np( &attr, PTHREAD_MUTEX_ERRORCHECK_NP );
# else
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_ERRORCHECK );
# endif
#endif
    if( pthread_mutex_init( p_mutex, &attr ) )
        abort();
    pthread_mutexattr_destroy( &attr );
}

/*****************************************************************************
 * vlc_mutex_init: initialize a recursive mutex (Do not use)
 *****************************************************************************/
void vlc_mutex_init_recursive( vlc_mutex_t *p_mutex )
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init( &attr );
#if defined (__GLIBC__) && (__GLIBC_MINOR__ < 6)
    pthread_mutexattr_setkind_np( &attr, PTHREAD_MUTEX_RECURSIVE_NP );
#else
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
#endif
    if( pthread_mutex_init( p_mutex, &attr ) )
        abort();
    pthread_mutexattr_destroy( &attr );
}


/**
 * Destroys a mutex. The mutex must not be locked.
 *
 * @param p_mutex mutex to destroy
 * @return always succeeds
 */
void vlc_mutex_destroy (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_destroy( p_mutex );
    VLC_THREAD_ASSERT ("destroying mutex");
}

#ifndef NDEBUG
# ifdef HAVE_VALGRIND_VALGRIND_H
#  include <valgrind/valgrind.h>
# else
#  define RUNNING_ON_VALGRIND (0)
# endif

void vlc_assert_locked (vlc_mutex_t *p_mutex)
{
    if (RUNNING_ON_VALGRIND > 0)
        return;
    assert (pthread_mutex_lock (p_mutex) == EDEADLK);
}
#endif

/**
 * Acquires a mutex. If needed, waits for any other thread to release it.
 * Beware of deadlocks when locking multiple mutexes at the same time,
 * or when using mutexes from callbacks.
 * This function is not a cancellation-point.
 *
 * @param p_mutex mutex initialized with vlc_mutex_init() or
 *                vlc_mutex_init_recursive()
 */
void vlc_mutex_lock (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_lock( p_mutex );
    VLC_THREAD_ASSERT ("locking mutex");
}

/**
 * Acquires a mutex if and only if it is not currently held by another thread.
 * This function never sleeps and can be used in delay-critical code paths.
 * This function is not a cancellation-point.
 *
 * <b>Beware</b>: If this function fails, then the mutex is held... by another
 * thread. The calling thread must deal with the error appropriately. That
 * typically implies postponing the operations that would have required the
 * mutex. If the thread cannot defer those operations, then it must use
 * vlc_mutex_lock(). If in doubt, use vlc_mutex_lock() instead.
 *
 * @param p_mutex mutex initialized with vlc_mutex_init() or
 *                vlc_mutex_init_recursive()
 * @return 0 if the mutex could be acquired, an error code otherwise.
 */
int vlc_mutex_trylock (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_trylock( p_mutex );

    if (val != EBUSY)
        VLC_THREAD_ASSERT ("locking mutex");
    return val;
}

/**
 * Releases a mutex (or crashes if the mutex is not locked by the caller).
 * @param p_mutex mutex locked with vlc_mutex_lock().
 */
void vlc_mutex_unlock (vlc_mutex_t *p_mutex)
{
    int val = pthread_mutex_unlock( p_mutex );
    VLC_THREAD_ASSERT ("unlocking mutex");
}

/*****************************************************************************
 * vlc_cond_init: initialize a condition variable
 *****************************************************************************/
void vlc_cond_init( vlc_cond_t *p_condvar )
{
    pthread_condattr_t attr;

    if (pthread_condattr_init (&attr))
        abort ();
#if !defined (_POSIX_CLOCK_SELECTION)
   /* Fairly outdated POSIX support (that was defined in 2001) */
# define _POSIX_CLOCK_SELECTION (-1)
#endif
#if (_POSIX_CLOCK_SELECTION >= 0)
    /* NOTE: This must be the same clock as the one in mtime.c */
    pthread_condattr_setclock (&attr, CLOCK_MONOTONIC);
#endif

    if (pthread_cond_init (p_condvar, &attr))
        abort ();
    pthread_condattr_destroy (&attr);
}

/**
 * Destroys a condition variable. No threads shall be waiting or signaling the
 * condition.
 * @param p_condvar condition variable to destroy
 */
void vlc_cond_destroy (vlc_cond_t *p_condvar)
{
    int val = pthread_cond_destroy( p_condvar );
    VLC_THREAD_ASSERT ("destroying condition");
}

/**
 * Wakes up one thread waiting on a condition variable, if any.
 * @param p_condvar condition variable
 */
void vlc_cond_signal (vlc_cond_t *p_condvar)
{
    int val = pthread_cond_signal( p_condvar );
    VLC_THREAD_ASSERT ("signaling condition variable");
}

/**
 * Wakes up all threads (if any) waiting on a condition variable.
 * @param p_cond condition variable
 */
void vlc_cond_broadcast (vlc_cond_t *p_condvar)
{
    pthread_cond_broadcast (p_condvar);
}

/**
 * Waits for a condition variable. The calling thread will be suspended until
 * another thread calls vlc_cond_signal() or vlc_cond_broadcast() on the same
 * condition variable, the thread is cancelled with vlc_cancel(), or the
 * system causes a "spurious" unsolicited wake-up.
 *
 * A mutex is needed to wait on a condition variable. It must <b>not</b> be
 * a recursive mutex. Although it is possible to use the same mutex for
 * multiple condition, it is not valid to use different mutexes for the same
 * condition variable at the same time from different threads.
 *
 * In case of thread cancellation, the mutex is always locked before
 * cancellation proceeds.
 *
 * The canonical way to use a condition variable to wait for event foobar is:
 @code
   vlc_mutex_lock (&lock);
   mutex_cleanup_push (&lock); // release the mutex in case of cancellation

   while (!foobar)
       vlc_cond_wait (&wait, &lock);

   --- foobar is now true, do something about it here --

   vlc_cleanup_run (); // release the mutex
  @endcode
 *
 * @param p_condvar condition variable to wait on
 * @param p_mutex mutex which is unlocked while waiting,
 *                then locked again when waking up.
 * @param deadline <b>absolute</b> timeout
 *
 * @return 0 if the condition was signaled, an error code in case of timeout.
 */
void vlc_cond_wait (vlc_cond_t *p_condvar, vlc_mutex_t *p_mutex)
{
    int val = pthread_cond_wait( p_condvar, p_mutex );
    VLC_THREAD_ASSERT ("waiting on condition");
}

/**
 * Waits for a condition variable up to a certain date.
 * This works like vlc_cond_wait(), except for the additional timeout.
 *
 * @param p_condvar condition variable to wait on
 * @param p_mutex mutex which is unlocked while waiting,
 *                then locked again when waking up.
 * @param deadline <b>absolute</b> timeout
 *
 * @return 0 if the condition was signaled, an error code in case of timeout.
 */
int vlc_cond_timedwait (vlc_cond_t *p_condvar, vlc_mutex_t *p_mutex,
                        mtime_t deadline)
{
#if defined(__APPLE__) && !defined(__powerpc__) && !defined( __ppc__ ) && !defined( __ppc64__ )
    /* mdate() is mac_absolute_time on OSX, which we must convert to do
     * the same base than gettimeofday() which pthread_cond_timedwait
     * relies on. */
    mtime_t oldbase = mdate();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    mtime_t newbase = (mtime_t)tv.tv_sec * 1000000 + (mtime_t) tv.tv_usec;
    deadline = deadline - oldbase + newbase;
#endif
    lldiv_t d = lldiv( deadline, CLOCK_FREQ );
    struct timespec ts = { d.quot, d.rem * (1000000000 / CLOCK_FREQ) };

    int val = pthread_cond_timedwait (p_condvar, p_mutex, &ts);
    if (val != ETIMEDOUT)
        VLC_THREAD_ASSERT ("timed-waiting on condition");
    return val;
}

/**
 * Allocates a thread-specific variable.
 * @param key where to store the thread-specific variable handle
 * @param destr a destruction callback. It is called whenever a thread exits
 * and the thread-specific variable has a non-NULL value.
 * @return 0 on success, a system error code otherwise. This function can
 * actually fail because there is a fixed limit on the number of
 * thread-specific variable in a process on most systems.
 */
int vlc_threadvar_create (vlc_threadvar_t *key, void (*destr) (void *))
{
    return pthread_key_create (key, destr);
}

void vlc_threadvar_delete (vlc_threadvar_t *p_tls)
{
    pthread_key_delete (*p_tls);
}

/**
 * Sets a thread-specific variable.
 * @param key thread-local variable key (created with vlc_threadvar_create())
 * @param value new value for the variable for the calling thread
 * @return 0 on success, a system error code otherwise.
 */
int vlc_threadvar_set (vlc_threadvar_t key, void *value)
{
    return pthread_setspecific (key, value);
}

/**
 * Gets the value of a thread-local variable for the calling thread.
 * This function cannot fail.
 * @return the value associated with the given variable for the calling
 * or NULL if there is no value.
 */
void *vlc_threadvar_get (vlc_threadvar_t key)
{
    return pthread_getspecific (key);
}

/**
 * Creates and starts new thread.
 *
 * @param p_handle [OUT] pointer to write the handle of the created thread to
 * @param entry entry point for the thread
 * @param data data parameter given to the entry point
 * @param priority thread priority value
 * @return 0 on success, a standard error code on error.
 */
int vlc_clone (vlc_thread_t *p_handle, void * (*entry) (void *), void *data,
               int priority)
{
    int ret;

    pthread_attr_t attr;
    pthread_attr_init (&attr);

    /* Block the signals that signals interface plugin handles.
     * If the LibVLC caller wants to handle some signals by itself, it should
     * block these before whenever invoking LibVLC. And it must obviously not
     * start the VLC signals interface plugin.
     *
     * LibVLC will normally ignore any interruption caused by an asynchronous
     * signal during a system call. But there may well be some buggy cases
     * where it fails to handle EINTR (bug reports welcome). Some underlying
     * libraries might also not handle EINTR properly.
     */
    sigset_t oldset;
    {
        sigset_t set;
        sigemptyset (&set);
        sigdelset (&set, SIGHUP);
        sigaddset (&set, SIGINT);
        sigaddset (&set, SIGQUIT);
        sigaddset (&set, SIGTERM);

        sigaddset (&set, SIGPIPE); /* We don't want this one, really! */
        pthread_sigmask (SIG_BLOCK, &set, &oldset);
    }

#if defined (_POSIX_PRIORITY_SCHEDULING) && (_POSIX_PRIORITY_SCHEDULING >= 0) \
 && defined (_POSIX_THREAD_PRIORITY_SCHEDULING) \
 && (_POSIX_THREAD_PRIORITY_SCHEDULING >= 0)
    {
        struct sched_param sp = { .sched_priority = priority, };
        int policy;

        if (sp.sched_priority <= 0)
            sp.sched_priority += sched_get_priority_max (policy = SCHED_OTHER);
        else
            sp.sched_priority += sched_get_priority_min (policy = SCHED_RR);

        pthread_attr_setschedpolicy (&attr, policy);
        pthread_attr_setschedparam (&attr, &sp);
    }
#else
    (void) priority;
#endif

    /* The thread stack size.
     * The lower the value, the less address space per thread, the highest
     * maximum simultaneous threads per process. Too low values will cause
     * stack overflows and weird crashes. Set with caution. Also keep in mind
     * that 64-bits platforms consume more stack than 32-bits one.
     *
     * Thanks to on-demand paging, thread stack size only affects address space
     * consumption. In terms of memory, threads only use what they need
     * (rounded up to the page boundary).
     *
     * For example, on Linux i386, the default is 2 mega-bytes, which supports
     * about 320 threads per processes. */
#define VLC_STACKSIZE (128 * sizeof (void *) * 1024)

#ifdef VLC_STACKSIZE
    ret = pthread_attr_setstacksize (&attr, VLC_STACKSIZE);
    assert (ret == 0); /* fails iif VLC_STACKSIZE is invalid */
#endif

    ret = pthread_create (p_handle, &attr, entry, data);
    pthread_sigmask (SIG_SETMASK, &oldset, NULL);
    pthread_attr_destroy (&attr);
    return ret;
}

/**
 * Marks a thread as cancelled. Next time the target thread reaches a
 * cancellation point (while not having disabled cancellation), it will
 * run its cancellation cleanup handler, the thread variable destructors, and
 * terminate. vlc_join() must be used afterward regardless of a thread being
 * cancelled or not.
 */
void vlc_cancel (vlc_thread_t thread_id)
{
    pthread_cancel (thread_id);
}

/**
 * Waits for a thread to complete (if needed), and destroys it.
 * This is a cancellation point; in case of cancellation, the join does _not_
 * occur.
 *
 * @param handle thread handle
 * @param p_result [OUT] pointer to write the thread return value or NULL
 * @return 0 on success, a standard error code otherwise.
 */
void vlc_join (vlc_thread_t handle, void **result)
{
    int val = pthread_join (handle, result);
    VLC_THREAD_ASSERT ("joining thread");
}

/**
 * Save the current cancellation state (enabled or disabled), then disable
 * cancellation for the calling thread.
 * This function must be called before entering a piece of code that is not
 * cancellation-safe, unless it can be proven that the calling thread will not
 * be cancelled.
 * @return Previous cancellation state (opaque value for vlc_restorecancel()).
 */
int vlc_savecancel (void)
{
    int state;
    int val = pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &state);

    VLC_THREAD_ASSERT ("saving cancellation");
    return state;
}

/**
 * Restore the cancellation state for the calling thread.
 * @param state previous state as returned by vlc_savecancel().
 * @return Nothing, always succeeds.
 */
void vlc_restorecancel (int state)
{
#ifndef NDEBUG
    int oldstate, val;

    val = pthread_setcancelstate (state, &oldstate);
    /* This should fail if an invalid value for given for state */
    VLC_THREAD_ASSERT ("restoring cancellation");

    if (oldstate != PTHREAD_CANCEL_DISABLE)
         vlc_thread_fatal ("restoring cancellation while not disabled", EINVAL,
                           __func__, __FILE__, __LINE__);
#else
    pthread_setcancelstate (state, NULL);
#endif
}

/**
 * Issues an explicit deferred cancellation point.
 * This has no effect if thread cancellation is disabled.
 * This can be called when there is a rather slow non-sleeping operation.
 * This is also used to force a cancellation point in a function that would
 * otherwise "not always" be a one (block_FifoGet() is an example).
 */
void vlc_testcancel (void)
{
    pthread_testcancel ();
}

void vlc_control_cancel (int cmd, ...)
{
    (void) cmd;
    assert (0);
}

#ifndef HAVE_POSIX_TIMER
/* We have no fallback currently. We'll just crash on timer API usage. */
static void timer_not_supported(void)
{
    fprintf(stderr, "*** Error: Timer API is not supported on this platform.\n");
    abort();
}
#endif

static void vlc_timer_do (union sigval val)
{
    vlc_timer_t *id = val.sival_ptr;
    id->func (id->data);
}

/**
 * Initializes an asynchronous timer.
 * @warning Asynchronous timers are processed from an unspecified thread, and
 * a timer is only serialized against itself.
 *
 * @param id pointer to timer to be initialized
 * @param func function that the timer will call
 * @param data parameter for the timer function
 * @return 0 on success, a system error code otherwise.
 */
int vlc_timer_create (vlc_timer_t *id, void (*func) (void *), void *data)
{
#ifdef HAVE_POSIX_TIMER
    struct sigevent ev;

    memset (&ev, 0, sizeof (ev));
    ev.sigev_notify = SIGEV_THREAD;
    ev.sigev_value.sival_ptr = id;
    ev.sigev_notify_function = vlc_timer_do;
    ev.sigev_notify_attributes = NULL;
    id->func = func;
    id->data = data;

#if (_POSIX_CLOCK_SELECTION >= 0)
    if (timer_create (CLOCK_MONOTONIC, &ev, &id->handle))
#else
    if (timer_create (CLOCK_REALTIME, &ev, &id->handle))
#endif
        return errno;

    return 0;
#else
    timer_not_supported();
    return 0;
#endif
}

/**
 * Destroys an initialized timer. If needed, the timer is first disarmed.
 * This function is undefined if the specified timer is not initialized.
 *
 * @warning This function <b>must</b> be called before the timer data can be
 * freed and before the timer callback function can be unloaded.
 *
 * @param timer to destroy
 */
void vlc_timer_destroy (vlc_timer_t *id)
{
#ifdef HAVE_POSIX_TIMER
    int val = timer_delete (id->handle);
    VLC_THREAD_ASSERT ("deleting timer");
#else
    timer_not_supported();
#endif
}

/**
 * Arm or disarm an initialized timer.
 * This functions overrides any previous call to itself.
 *
 * @note A timer can fire later than requested due to system scheduling
 * limitations. An interval timer can fail to trigger sometimes, either because
 * the system is busy or suspended, or because a previous iteration of the
 * timer is still running. See also vlc_timer_getoverrun().
 *
 * @param id initialized timer pointer
 * @param absolute the timer value origin is the same as mdate() if true,
 *                 the timer value is relative to now if false.
 * @param value zero to disarm the timer, otherwise the initial time to wait
 *              before firing the timer.
 * @param interval zero to fire the timer just once, otherwise the timer
 *                 repetition interval.
 */
void vlc_timer_schedule (vlc_timer_t *id, bool absolute,
                         mtime_t value, mtime_t interval)
{
#ifdef HAVE_POSIX_TIMER
    lldiv_t vad = lldiv (value, CLOCK_FREQ);
    lldiv_t itd = lldiv (interval, CLOCK_FREQ);
    struct itimerspec it = {
        .it_interval = {
            .tv_sec = itd.quot,
            .tv_nsec = (1000000000 / CLOCK_FREQ) * itd.rem,
        },
        .it_value = {
            .tv_sec = vad.quot,
            .tv_nsec = (1000000000 / CLOCK_FREQ) * vad.rem,
        },
    };
    int flags = absolute ? TIMER_ABSTIME : 0;

    int val = timer_settime (id->handle, flags, &it, NULL);
    VLC_THREAD_ASSERT ("scheduling timer");
#else
    timer_not_supported();
#endif
}

/**
 * @param id initialized timer pointer
 * @return the timer overrun counter, i.e. the number of times that the timer
 * should have run but did not since the last actual run. If all is well, this
 * is zero.
 */
unsigned vlc_timer_getoverrun (const vlc_timer_t *id)
{
#ifdef HAVE_POSIX_TIMER
    int val = timer_getoverrun (id->handle);
#ifndef NDEBUG
    if (val == -1)
    {
        val = errno;
        VLC_THREAD_ASSERT ("fetching timer overrun counter");
    }
#endif
    return val;
#else
    timer_not_supported();
    return 0;
#endif
}