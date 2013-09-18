#ifndef Py_PYPOOLEDLOCK_H
#define Py_PYPOOLEDLOCK_H
#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

#ifndef WITH_FREE_THREAD

#define Py_DECLARE_POOLED_LOCK
#define Py_POOLED_INIT(ob)
#define Py_POOLED_LOCK(ob)		0
#define Py_POOLED_LOCK_TEST(ob,v)
#define Py_POOLED_UNLOCK(ob)
#define Py_POOLED_LAZY_UNLOCK(ob)
#define Py_POOLED_LAZY_DONE(ob)

#else
    
#define Py_DECLARE_POOLED_LOCK		PyPooledLock *ob_lock;
#define Py_POOLED_INIT(ob)		((ob)->ob_lock = NULL)
#define Py_POOLED_LOCK(ob)		PyPooledLock_Lock(&(ob)->ob_lock)
#define Py_POOLED_LOCK_TEST(ob,v)	if ( Py_POOLED_LOCK(ob) ) return (v); else
#define Py_POOLED_UNLOCK(ob)		PyPooledLock_Unlock(&(ob)->ob_lock)
#define Py_POOLED_LAZY_UNLOCK(ob)	PyPooledLock_LazyUnlock(&(ob)->ob_lock)
#define Py_POOLED_LAZY_DONE(ob)		PyPooledLock_LazyDone(&(ob)->ob_lock)


typedef struct PyPooledLock_s PyPooledLock;

/* install/use a lock from the pool at the specified location.
   returns 1 if an error occurs. */
extern int PyPooledLock_Lock Py_PROTO((PyPooledLock **ppl));

/* unlock and free the lock at the specified location */
extern void PyPooledLock_Unlock Py_PROTO((PyPooledLock **ppl));

/* unlock the lock at the specified location, leaving it for re-use */
extern void PyPooledLock_LazyUnlock Py_PROTO((PyPooledLock **ppl));

/* notify that the lazy usage is done; the lock can be freed */
extern void PyPooledLock_LazyDone Py_PROTO((PyPooledLock **ppl));

/* initialize the pooled lock subsystem */
extern void PyPooledLock_Init Py_PROTO((void));

#endif /* WITH_FREE_THREAD */

#ifdef __cplusplus
}
#endif
#endif /* !Py_PYPOOLEDLOCK_H */
