#ifndef Py_PYMUTEX_H
#define Py_PYMUTEX_H
#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

/*
** Free threaded or not, the following macros should be provided:
**
** void Py_CRIT_LOCK(void)
** void Py_CRIT_UNLOCK(void)
**
**    These macros are used to mark a critical section in the code.  They
**    use a single non-reentrant lock, so care must be taken.
**
**    When free threading is not enabled, these macros evaluate to nothing.
*/


#ifndef WITH_FREE_THREAD

#define Py_CRIT_LOCK()
#define Py_CRIT_UNLOCK()


#else /* WITH_FREE_THREAD */

extern void PyMutex_Init Py_PROTO((void));

typedef struct PyMutex_s PyMutex;

extern PyMutex * PyMutex_New Py_PROTO((void));
extern void PyMutex_Free Py_PROTO((PyMutex *));

/* extern void PyMutex_Lock Py_PROTO((PyMutex *));
   extern void PyMutex_Unlock Py_PROTO((PyMutex *));

   extern int Py_SafeIncr Py_PROTO((int *));
   extern int Py_SafeDecr Py_PROTO((int *)); */

/* mutexes for use by the various subsystems */
extern DL_IMPORT(PyMutex *) _Py_RefMutex;
extern DL_IMPORT(PyMutex *) _Py_ListMutex;
extern DL_IMPORT(PyMutex *) _Py_MappingMutex;
extern DL_IMPORT(PyMutex *) _Py_CritMutex;

#define Py_CRIT_LOCK()		PyMutex_Lock(_Py_CritMutex)
#define Py_CRIT_UNLOCK()	PyMutex_Unlock(_Py_CritMutex)


#ifdef _POSIX_THREADS

#include <pthread.h>

struct PyMutex_s
{
    pthread_mutex_t	mut;
};
#define _PYMUTEX_INIT(pm)	pthread_mutex_init(&(pm)->mut, NULL)
#define _PYMUTEX_FREE(pm)	pthread_mutex_destroy(&(pm)->mut)

#define PyMutex_Lock(pm)	pthread_mutex_lock(&(pm)->mut)
#define PyMutex_Unlock(pm)	pthread_mutex_unlock(&(pm)->mut)

/* PTHREADS uses the default versions of Py_SafeXXXX() */

/* these mutexes will (probably) deadlock a thread */
#undef PYMUTEX_REENTRANT

#endif /* _POSIX_THREADS */


#ifdef NT_THREADS

#include <windows.h>
#ifdef IN
#undef IN		/* mucks up Python's defn... */
#endif

struct PyMutex_s
{
    CRITICAL_SECTION	cs;
};
#define _PYMUTEX_INIT(pm)	InitializeCriticalSection(&(pm)->cs)
#define _PYMUTEX_FREE(pm)	DeleteCriticalSection(&(pm)->cs)

#define PyMutex_Lock(pm)	EnterCriticalSection(&(pm)->cs)
#define PyMutex_Unlock(pm)	LeaveCriticalSection(&(pm)->cs)

/* we'll say sizeof(int) == sizeof(long) */
#define Py_SafeIncr(pint)	InterlockedIncrement((long *)(pint))
#define Py_SafeDecr(pint)	InterlockedDecrement((long *)(pint))

/* these mutexes will not deadlock a thread */
#define PYMUTEX_REENTRANT

#endif /* NT_THREADS */

/*
** If definitions weren't provided, then provide some defaults
*/
#ifndef Py_SafeIncr

#define Py_SafeIncr(pint)	_Py_SafeIncr(pint)
#define Py_SafeDecr(pint)	_Py_SafeDecr(pint)

/* get the _Py_SafeXXXX() funcs */
#define _Py_NEED_SAFE_FUNCS

#endif /* Py_SafeIncr */

#endif /* WITH_FREE_THREAD */

#ifdef __cplusplus
}
#endif
#endif /* !Py_PYMUTEX_H */
