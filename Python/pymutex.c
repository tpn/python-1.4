#include "config.h"

#ifdef WITH_FREE_THREAD

#include "Python.h"
#include "pymutex.h"

/* mutexes for the subsystems */
PyMutex * _Py_RefMutex;
PyMutex * _Py_ListMutex;
PyMutex * _Py_MappingMutex;
PyMutex * _Py_CritMutex;


void PyMutex_Init()
{
    _Py_RefMutex = PyMutex_New();
    _Py_ListMutex = PyMutex_New();
    _Py_MappingMutex = PyMutex_New();
    _Py_CritMutex = PyMutex_New();

    if ( !_Py_RefMutex || !_Py_ListMutex ||
	 !_Py_MappingMutex || !_Py_CritMutex )
	Py_FatalError("could not allocate mutexes");
}

PyMutex * PyMutex_New()
{
    PyMutex *pm;

    pm = (PyMutex *)malloc(sizeof(*pm));
    if ( !pm )
	PyErr_NoMemory();
    else
	_PYMUTEX_INIT(pm);
    return pm;
}

void PyMutex_Free(pm)
    PyMutex *pm;
{
    _PYMUTEX_FREE(pm);
    free(pm);
}

#ifdef _Py_NEED_SAFE_FUNCS

/*
** Provide default versions of these functions
*/
int _Py_SafeIncr(pint)
    int *pint;
{
    int result;

    PyMutex_Lock(_Py_RefMutex);
    result = ++*pint;
    PyMutex_Unlock(_Py_RefMutex);
    return result;
}

int _Py_SafeDecr(pint)
    int *pint;
{
    int result;

    PyMutex_Lock(_Py_RefMutex);
    result = --*pint;
    PyMutex_Unlock(_Py_RefMutex);
    return result;
}

#endif /* _Py_NEED_SAFE_FUNCS */

#endif /* WITH_FREE_THREAD */
