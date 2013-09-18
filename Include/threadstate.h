#ifndef Py_THREADSTATE_H
#define Py_THREADSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

/* ### it would be nice if frameobject.h included compile.h itself... */
#include "compile.h"
#include "frameobject.h"

typedef struct PyThreadState_s
{
    PyFrameObject *		current_frame;		/* ceval.c */
    int				recursion_depth;	/* ceval.c */
    int				interp_ticker;		/* ceval.c */
    int				tracing;		/* ceval.c */

    PyObject *			sys_profilefunc;	/* sysmodule.c */
    PyObject *			sys_tracefunc;		/* sysmodule.c */
    int				sys_checkinterval;	/* sysmodule.c */

    PyObject *			last_exception;		/* errors.c */
    PyObject *			last_exc_val;		/* errors.c */
    PyObject *			last_traceback;		/* traceback.c */

    PyObject *			sort_comparefunc;	/* listobject.c */

    char			work_buf[120];		/* <anywhere> */

    int				c_error;		/* complexobject.c */

} PyThreadState;

extern PyThreadState *PyThreadState_Get Py_PROTO((void));

extern void PyThreadState_New Py_PROTO((void));
extern void PyThreadState_Free Py_PROTO((void));

extern int PyThreadState_Ensure Py_PROTO((void));

extern void _PyThreadState_Init Py_PROTO((void));

#ifdef __cplusplus
}
#endif
#endif /* !Py_THREADSTATE_H */
