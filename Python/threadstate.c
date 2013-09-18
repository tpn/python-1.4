#include "Python.h"
#include "threadstate.h"
#include "pymutex.h"

typedef struct PyThreadStateLL_s
{
    long			thread_id;
    struct PyThreadStateLL_s *	next;

    PyThreadState		state;

} PyThreadStateLL;

/*
** states will always maintain read-consistency, but must be mutexed for
** write-consistency.
**
** We will shuffle the "current" thread to the front of the list.  While
** we are traversing the list, other threads may move their states up to
** the front, but this movement will not affect the finding of our state.
**
** INVARIANT: p->next will never be changed to point to a structure that
**            is "earlier" in the linked list.
**
** INVARIANT: a thread will never move another thread's state.
**
** COROLLARY: progressing through the links will eventually bring you to
**            the proper state structure.
**
** INVARIANT: modification of "states" or p->next will occur only while
**            the mutex is held
*/
static PyThreadStateLL * volatile states;

#ifdef WITH_FREE_THREAD
static PyMutex *states_mutex;
#endif

#ifdef WITH_THREAD
static long main_thread;
#endif


PyThreadState *PyThreadState_Get()
{
#ifndef WITH_THREAD

    return &states->state;

#else

    long thread_id = get_thread_ident();
    PyThreadStateLL *pts = states;
    PyThreadStateLL *prev;

    /* fast-path */
    if ( pts->thread_id == thread_id )
    {
	return &pts->state;
    }

    while ( 1 )
    {
	prev = pts;
	pts = pts->next;
	if ( !pts )
	    Py_FatalError("could not find thread state");

	if ( pts->thread_id == thread_id )
	{
	    /*
	    ** Move our state structure to the head of the list to optimize
	    ** for the next read for this thread.
	    */

#ifdef WITH_FREE_THREAD
	    PyMutex_Lock(states_mutex);

	    if ( prev->next != pts )
	    {
		/* damn. another thread moved the prior threadstate. */
		prev = states;
		while ( prev->next != pts )
		    prev = prev->next;

		/* we've got the lock, so it won't move again... */
	    }
#endif

	    /*
	    ** Point around our state to the next one. This removes us
	    ** from the list, but nobody else cares.
	    */
	    prev->next = pts->next;

	    /*
	    ** Get us ready to go into the front of the list, then insert
	    ** us there.  Other threads may or may not see our state when
	    ** they begin a lookup; it doesn't matter which.
	    */
	    pts->next = states;
	    states = pts;

#ifdef WITH_FREE_THREAD
	    PyMutex_Unlock(states_mutex);
#endif

	    return &pts->state;
	}
    }

#endif /* WITH_THREAD */
}

void PyThreadState_New()
{
    PyThreadStateLL *pts;

    pts = (PyThreadStateLL *)malloc(sizeof(*pts));
    memset(pts, 0, sizeof(*pts));

#ifdef WITH_THREAD
    pts->thread_id = get_thread_ident();

    if ( pts->thread_id != main_thread )
    {
	PyThreadStateLL *ptsMain;

	/* find the main thread */
	ptsMain = states;
	while ( ptsMain->thread_id != main_thread )
	{
	    ptsMain = ptsMain->next;
	    if ( ptsMain == NULL )
		Py_FatalError("could not find main thread state");
	}

	/* inherit some values from the main thread */

	/* we need a lock since we're dealing with another thread's data */
	Py_CRIT_LOCK();
	pts->state.sys_profilefunc = ptsMain->state.sys_profilefunc;
	Py_XINCREF(pts->state.sys_profilefunc);
	pts->state.sys_tracefunc = ptsMain->state.sys_tracefunc;
	Py_XINCREF(pts->state.sys_tracefunc);
	Py_CRIT_UNLOCK();

	pts->state.sys_checkinterval = ptsMain->state.sys_checkinterval;
    }
    else
#endif
    {
	pts->state.sys_checkinterval = 10;
    }

#ifdef WITH_FREE_THREAD
    /* use the mutex because we're tweaking "states" */
    PyMutex_Lock(states_mutex);
#endif

    pts->next = states;
    states = pts;

#ifdef WITH_FREE_THREAD
    PyMutex_Unlock(states_mutex);
#endif
}

void PyThreadState_Free()
{
    PyThreadStateLL *pts;

#ifndef WITH_THREAD

    pts = states;

#else

    long thread_id = get_thread_ident();
    PyThreadStateLL *prev = NULL;

    /* never throw out the main thread state */
    if ( thread_id == main_thread )
	return;

#ifdef WITH_FREE_THREAD
    /* use the mutex because we modify states or p->next */
    PyMutex_Lock(states_mutex);
#endif

    pts = states;
    while ( pts && pts->thread_id != thread_id )
    {
	prev = pts;
	pts = pts->next;
    }
    if ( !pts )
    {
	/* ### unlock the mutex first? */
	Py_FatalError("could not find thread state for freeing");
    }

    if ( !prev )
	states = pts->next;
    else
	prev->next = pts->next;

#ifdef WITH_FREE_THREAD
    PyMutex_Unlock(states_mutex);
#endif

#endif /* WITH_THREAD */

    Py_XDECREF(pts->state.current_frame);
    Py_XDECREF(pts->state.last_exception);
    Py_XDECREF(pts->state.last_exc_val);
    Py_XDECREF(pts->state.last_traceback);
    free(pts);
}

int PyThreadState_Ensure()
{
#ifdef WITH_THREAD

    long thread_id = get_thread_ident();
    PyThreadStateLL *pts = states;

    while ( pts && pts->thread_id != thread_id )
	pts = pts->next;

    /* if we found it, then we don't have to create a thread state */
    if ( pts )
	return 0;

    PyThreadState_New();

    /* we created a new thread state */
    return 1;

#else

    /* only the main thread state exists, which we didn't create */
    return 0;

#endif /* WITH_THREAD */
}

void _PyThreadState_Init()
{
#ifdef WITH_FREE_THREAD
    states_mutex = PyMutex_New();
    if ( !states_mutex )
	Py_FatalError("could not allocate threadstate mutex");
#endif

#ifdef WITH_THREAD
    main_thread = get_thread_ident();
#endif

    /* create the main thread's thread state */
    PyThreadState_New();
}
