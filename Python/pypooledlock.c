#include "config.h"

#ifdef WITH_FREE_THREAD

#include "Python.h"
#include "pypooledlock.h"
#include "pymutex.h"

/* configurable option? */
#define WITH_DEADLOCK_DETECTION

#ifdef WITH_DEADLOCK_DETECTION
#ifndef PYMUTEX_REENTRANT
#define USE_DEADLOCK_DETECTION
#endif
#endif

struct PyPooledLock_s
{
    PyMutex		mutex;		/* embed the mutex structure */
    int			waiting;	/* how many waiting for the lock? */
    PyPooledLock *	next;		/* next in queue */

#ifdef USE_DEADLOCK_DETECTION
    long		thread_id;	/* id of thread holding lock */
#endif
};

static PyPooledLock *	pooled_lock_queue;
static PyMutex *	pooled_lock_mutex;

#define PyPooledLock_LOCK()	PyMutex_Lock(pooled_lock_mutex)
#define PyPooledLock_UNLOCK()	PyMutex_Unlock(pooled_lock_mutex)

int PyPooledLock_Lock(ppl)
    PyPooledLock **ppl;
{
    PyPooledLock *pl;

    PyPooledLock_LOCK();
    if ( *ppl )
    {
	++(pl = *ppl)->waiting;

#ifdef USE_DEADLOCK_DETECTION
	if ( get_thread_ident() == pl->thread_id ) {
	    --pl->waiting;
	    PyPooledLock_UNLOCK();
	    PyErr_SetString(PyExc_SystemError, "deadlock detected");
	    return 1;
	}
#endif

	/* INVARIANT: waiting >=1; <pl> will remain valid */
	PyPooledLock_UNLOCK();

	PyMutex_Lock(&pl->mutex);
    }
    else if ( pooled_lock_queue )
    {
	pl = *ppl = pooled_lock_queue;
	pooled_lock_queue = pl->next;
	/* ASSERT: waiting==1, mutex locked, deadlock thread==0. */

	PyPooledLock_UNLOCK();
    }
    else
    {
	pl = *ppl = malloc(sizeof(PyPooledLock));
	if ( pl == NULL )
	{
	    PyPooledLock_UNLOCK();
	    PyErr_NoMemory();
	    return 1;
	}

	_PYMUTEX_INIT(&pl->mutex);
	pl->waiting = 1;

#ifdef USE_DEADLOCK_DETECTION
	/* somebody might look at this before we acquire and set it */
	pl->thread_id = 0;
#endif

	/* INVARIANT: waiting >=1; <pl> will remain valid */
	PyPooledLock_UNLOCK();

	PyMutex_Lock(&pl->mutex);
    }

#ifdef USE_DEADLOCK_DETECTION
    /* we now have the lock... */
    pl->thread_id = get_thread_ident();
#endif

    return 0;
}

void PyPooledLock_Unlock(ppl)
    PyPooledLock **ppl;
{
    PyPooledLock *pl = *ppl;

    PyPooledLock_LOCK();
    if ( pl->waiting == 1 )
    {
	/* put the lock into the queue w/ waiting==1 and the mutex locked */
	/* (and the deadlock thread_id == 0) */
	pl->next = pooled_lock_queue;
	pooled_lock_queue = pl;

#ifdef USE_DEADLOCK_DETECTION
	/* initialize for when lock is pulled out of queue. */
	pl->thread_id = 0;
#endif

	*ppl = NULL;
	PyPooledLock_UNLOCK();
    }
    else
    {
	--pl->waiting;

	/*
	** After this unlock, there will be at least one person blocked on
	** the pooled lock.  They will not call unlock until after they
	** unblock; therefore, they cannot invalidate <pl> before we
	** unblock them later.
	*/
	PyPooledLock_UNLOCK();

#ifdef USE_DEADLOCK_DETECTION
	pl->thread_id = 0;	/* ### is this a valid NULL thread_id? */
#endif

	/* unblock the waiter(s) */
	PyMutex_Unlock(&pl->mutex);
    }
}

void PyPooledLock_LazyUnlock(ppl)
    PyPooledLock **ppl;
{
    PyPooledLock *pl = *ppl;

    /*
    ** After this decrement, another thread must acquire the mutex lock
    ** before it can do anything further.  That won't be possible until
    ** we unlock the mutex.
    */
    Py_SafeDecr(&pl->waiting);

#ifdef USE_DEADLOCK_DETECTION
    pl->thread_id = 0;	/* ### is this a valid NULL thread_id? */
#endif

    /* now others can acquire this... */
    PyMutex_Unlock(&pl->mutex);
}

void PyPooledLock_LazyDone(ppl)
    PyPooledLock **ppl;
{
    PyPooledLock *pl;

    PyPooledLock_LOCK();
    if ( (pl = *ppl) != NULL && pl->waiting == 0 )
    {
	/* put the lock into the queue w/ waiting==1 and the mutex locked */
	/* (and the deadlock thread_id == 0) */
	pl->waiting = 1;
	PyMutex_Lock(&pl->mutex);

	pl->next = pooled_lock_queue;
	pooled_lock_queue = pl;

	*ppl = NULL;
    }
    PyPooledLock_UNLOCK();
}

void PyPooledLock_Init()
{
    pooled_lock_queue = NULL;
    pooled_lock_mutex = PyMutex_New();
}

#endif /* WITH_FREE_THREAD */
