/***********************************************************
Copyright 1991-1995 by Stichting Mathematisch Centrum, Amsterdam,
The Netherlands.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the names of Stichting Mathematisch
Centrum or CWI or Corporation for National Research Initiatives or
CNRI not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

While CWI is the initial source for this software, a modified version
is made available by the Corporation for National Research Initiatives
(CNRI) at the Internet address ftp://ftp.python.org.

STICHTING MATHEMATISCH CENTRUM AND CNRI DISCLAIM ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL STICHTING MATHEMATISCH
CENTRUM OR CNRI BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

/* List object implementation */

#include "allobjects.h"
#include "modsupport.h"
#include "ceval.h"
#include "threadstate.h"
#ifdef STDC_HEADERS
#include <stddef.h>
#else
#include <sys/types.h>		/* For size_t */
#endif

#define Py_LIST_LOCK(op)	Py_POOLED_LOCK((listobject *)(op))
#define Py_LIST_LOCK_TEST(op,v)	Py_POOLED_LOCK_TEST((listobject *)(op), (v))
#define Py_LIST_UNLOCK(op)	Py_POOLED_UNLOCK((listobject *)(op))
#define Py_LIST_LAZY_UNLOCK(op)	Py_POOLED_LAZY_UNLOCK((listobject *)(op))
#define Py_LIST_LAZY_DONE(op)	Py_POOLED_LAZY_DONE((listobject *)(op))

#define ROUNDUP(n, block) ((((n)+(block)-1)/(block))*(block))

/* some systems define a roundup() macro */
#ifdef roundup
#undef roundup
#endif

static int
roundup(n)
	int n;
{
	if (n < 500)
		return ROUNDUP(n, 10);
	else
		return ROUNDUP(n, 100);
}

#define NRESIZE(var, type, nitems) RESIZE(var, type, roundup(nitems))

object *
newlistobject(size)
	int size;
{
	int i;
	listobject *op;
	size_t nbytes;
	if (size < 0) {
		err_badcall();
		return NULL;
	}
	nbytes = size * sizeof(object *);
	/* Check for overflow */
	if (nbytes / sizeof(object *) != size) {
		return err_nomem();
	}
	op = (listobject *) malloc(sizeof(listobject));
	if (op == NULL) {
		return err_nomem();
	}
	if (size <= 0) {
		op->ob_item = NULL;
	}
	else {
		op->ob_item = (object **) malloc(nbytes);
		if (op->ob_item == NULL) {
			free((ANY *)op);
			return err_nomem();
		}
	}
	op->ob_type = &Listtype;
	op->ob_size = size;
	Py_POOLED_INIT(op);
	for (i = 0; i < size; i++)
		op->ob_item[i] = NULL;
	NEWREF(op);
	return (object *) op;
}

int
getlistsize(op)
	object *op;
{
	if (!is_listobject(op)) {
		err_badcall();
		return -1;
	}
	else
		return ((listobject *)op) -> ob_size;
}

static object *indexerr;

/* WARNING: THIS FUNCTION IS *NOT* THREAD SAFE */
object *
getlistitem(op, i)
	object *op;
	int i;
{
	if (!is_listobject(op)) {
		err_badcall();
		return NULL;
	}
	if (i < 0 || i >= ((listobject *)op) -> ob_size) {
		if (indexerr == NULL)
			indexerr = newstringobject("list index out of range");
		err_setval(IndexError, indexerr);
		return NULL;
	}
	return ((listobject *)op) -> ob_item[i];
}

int
setlistitem(op, i, newitem)
	register object *op;
	register int i;
	register object *newitem;
{
	register object *olditem;
	register object **p;
	if (!is_listobject(op)) {
		XDECREF(newitem);
		err_badcall();
		return -1;
	}
	Py_LIST_LOCK_TEST(op, -1);
	if (i < 0 || i >= ((listobject *)op) -> ob_size) {
		Py_LIST_UNLOCK(op);
		XDECREF(newitem);
		err_setstr(IndexError, "list assignment index out of range");
		return -1;
	}
	p = ((listobject *)op) -> ob_item + i;
	olditem = *p;
	*p = newitem;
	Py_LIST_UNLOCK(op);
	XDECREF(olditem);
	return 0;
}

static int
ins1(self, where, v)
	listobject *self;
	int where;
	object *v;
{
	int i;
	object **items;
	if (v == NULL) {
		err_badcall();
		return -1;
	}
	Py_LIST_LOCK_TEST(self, -1);
	items = self->ob_item;
	NRESIZE(items, object *, self->ob_size+1);
	if (items == NULL) {
		Py_LIST_UNLOCK(self);
		err_nomem();
		return -1;
	}
	if (where < 0)
		where = 0;
	if (where > self->ob_size)
		where = self->ob_size;
	for (i = self->ob_size; --i >= where; )
		items[i+1] = items[i];
	INCREF(v);
	items[where] = v;
	self->ob_item = items;
	self->ob_size++;
	Py_LIST_UNLOCK(self);
	return 0;
}

int
inslistitem(op, where, newitem)
	object *op;
	int where;
	object *newitem;
{
	if (!is_listobject(op)) {
		err_badcall();
		return -1;
	}
	return ins1((listobject *)op, where, newitem);
}

int
addlistitem(op, newitem)
	object *op;
	object *newitem;
{
	if (!is_listobject(op)) {
		err_badcall();
		return -1;
	}
	return ins1((listobject *)op,
		(int) ((listobject *)op)->ob_size, newitem);
}


/* Methods */

static void
list_dealloc(op)
	listobject *op;
{
	int i;
	if (op->ob_item != NULL) {
		for (i = 0; i < op->ob_size; i++) {
			XDECREF(op->ob_item[i]);
		}
		free((ANY *)op->ob_item);
	}
	Py_POOLED_LAZY_DONE(op);
	free((ANY *)op);
}

static int
list_print(op, fp, flags)
	listobject *op;
	FILE *fp;
	int flags;
{
	int i;
	fprintf(fp, "[");
	for (i = 0; i < op->ob_size; i++) {
		object *o;
		int rc;

		if (i > 0)
			fprintf(fp, ", ");

#ifdef WITH_FREE_THREAD
		Py_LIST_LOCK_TEST(op, -1);
		if ( i >= op->ob_size ) {
			Py_LIST_UNLOCK(op);
			break;
		}
		o = op->ob_item[i];
		Py_INCREF(o);
		Py_LIST_LAZY_UNLOCK(op);

		rc = printobject(o, fp, 0);
		Py_DECREF(o);
#else
		rc = printobject(op->ob_item[i], fp, 0);
#endif

		if ( rc != 0 ) {
			Py_LIST_LAZY_DONE(op);
			return -1;
		}
	}
	fprintf(fp, "]");
	Py_LIST_LAZY_DONE(op);
	return 0;
}

static object *
list_repr(v)
	listobject *v;
{
	object *s, *comma;
	int i;
	s = newstringobject("[");
	comma = newstringobject(", ");
	for (i = 0; i < v->ob_size && s != NULL; i++) {
		if (i > 0)
			joinstring(&s, comma);
#ifdef WITH_FREE_THREAD
		if ( Py_LIST_LOCK(v) ) {
			Py_XDECREF(s);
			s = NULL;
			break;
		} else if ( i < v->ob_size ) {
			object *o = v->ob_item[i];
			Py_INCREF(o);
			Py_LIST_LAZY_UNLOCK(v);

			joinstring_decref(&s, reprobject(o));
			Py_DECREF(o);
		} else {
			Py_LIST_UNLOCK(v);
		}
#else
		joinstring_decref(&s, reprobject(v->ob_item[i]));
#endif
	}
	Py_LIST_LAZY_DONE(v);
	XDECREF(comma);
	joinstring_decref(&s, newstringobject("]"));
	return s;
}

static int
list_compare(v, w)
	listobject *v, *w;
{
#ifndef WITH_FREE_THREAD

	int len = (v->ob_size < w->ob_size) ? v->ob_size : w->ob_size;
	int i;
	for (i = 0; i < len; i++) {
		int cmp = cmpobject(v->ob_item[i], w->ob_item[i]);
		if (cmp != 0)
			return cmp;
	}
	return v->ob_size - w->ob_size;

#else

	int len = (v->ob_size < w->ob_size) ? v->ob_size : w->ob_size;
	int i;
	for (i = 0; i < len; i++) {
		object *o1;
		object *o2;
		int cmp;

		Py_LIST_LOCK_TEST(v, -1);
		if ( i >= v->ob_size ) {
			Py_LIST_UNLOCK(v);
			break;
		}
		o1 = v->ob_item[i];
		Py_INCREF(o1);
		Py_LIST_LAZY_UNLOCK(v);

		Py_LIST_LOCK_TEST(w, -1);
		if ( i >= w->ob_size ) {
			Py_LIST_UNLOCK(w);
			break;
		}
		o2 = w->ob_item[i];
		Py_INCREF(o2);
		Py_LIST_LAZY_UNLOCK(w);

		cmp = cmpobject(o1, o2);
		Py_DECREF(o1);
		Py_DECREF(o2);
		if (cmp != 0) {
			Py_LIST_LAZY_DONE(v);
			Py_LIST_LAZY_DONE(w);
			return cmp;
		}
	}
	Py_LIST_LAZY_DONE(v);
	Py_LIST_LAZY_DONE(w);
	return v->ob_size - w->ob_size;

#endif
}

static int
list_length(a)
	listobject *a;
{
	return a->ob_size;
}

static object *
list_item(a, i)
	listobject *a;
	int i;
{
	object *o;
	Py_LIST_LOCK_TEST(a, NULL);
	if (i < 0 || i >= a->ob_size) {
		Py_LIST_UNLOCK(a);
		if (indexerr == NULL)
			indexerr = newstringobject("list index out of range");
		err_setval(IndexError, indexerr);
		return NULL;
	}
	o = a->ob_item[i];
	INCREF(o);
	Py_LIST_UNLOCK(a);
	return o;
}

static object *
list_slice(a, ilow, ihigh)
	listobject *a;
	int ilow, ihigh;
{
	listobject *np;
	int i;
	int size = a->ob_size;

	Py_LIST_LOCK_TEST(a, NULL);
	if (ilow < 0)
		ilow = 0;
	else if (ilow > size)
		ilow = size;
	if (ihigh < 0)
		ihigh = 0;
	if (ihigh < ilow)
		ihigh = ilow;
	else if (ihigh > size)
		ihigh = size;
	Py_LIST_LAZY_UNLOCK(a);
	np = (listobject *) newlistobject(ihigh - ilow);
	if (np == NULL) {
		Py_LIST_LAZY_DONE(a);
		return NULL;
	}
	if ( Py_LIST_LOCK(a) ) {
		Py_DECREF(np);
		return NULL;
	}
	/* see if self changed while it was unlocked... */
	if ( a->ob_size < size ) {
		if (ilow > a->ob_size)
			ilow = a->ob_size;
		if (ihigh > a->ob_size)
			ihigh = a->ob_size;
		np->ob_size = ihigh - ilow;
	}
	for (i = ilow; i < ihigh; i++) {
		object *v = a->ob_item[i];
		INCREF(v);
		np->ob_item[i - ilow] = v;
	}
	Py_LIST_UNLOCK(a);
	return (object *)np;
}

object *
getlistslice(a, ilow, ihigh)
	object *a;
	int ilow, ihigh;
{
	if (!is_listobject(a)) {
		err_badcall();
		return NULL;
	}
	return list_slice((listobject *)a, ilow, ihigh);
}

static object *
list_concat(a, bb)
	listobject *a;
	object *bb;
{
	int asize;
	int bsize;
	int i;
	listobject *np;
	if (!is_listobject(bb)) {
		err_badarg();
		return NULL;
	}
#define b ((listobject *)bb)
	asize = a->ob_size;
	bsize = b->ob_size;
	np = (listobject *) newlistobject(asize + bsize);
	if (np == NULL) {
		return NULL;
	}
	if ( Py_LIST_LOCK(a) ) {
		Py_DECREF(np);
		return NULL;
	}
	if ( Py_LIST_LOCK(b) ) {
		Py_LIST_UNLOCK(a);
		Py_DECREF(np);
		return NULL;
	}
	if ( a->ob_size < asize ) {
		asize = a->ob_size;
		np->ob_size = asize + bsize;
	}
	if ( b->ob_size < bsize ) {
		bsize = b->ob_size;
		np->ob_size = asize + bsize;
	}
	for (i = 0; i < asize; i++) {
		object *v = a->ob_item[i];
		INCREF(v);
		np->ob_item[i] = v;
	}
	for (i = 0; i < bsize; i++) {
		object *v = b->ob_item[i];
		INCREF(v);
		np->ob_item[i + asize] = v;
	}
	Py_LIST_UNLOCK(a);
	Py_LIST_UNLOCK(b);
	return (object *)np;
#undef b
}

static object *
list_repeat(a, n)
	listobject *a;
	int n;
{
	int i, j;
	int size;
	listobject *np;
	object **p;
	if (n < 0)
		n = 0;
	size = a->ob_size;
	np = (listobject *) newlistobject(size * n);
	if (np == NULL)
		return NULL;
	if ( Py_LIST_LOCK(a) ) {
		Py_DECREF(np);
		return NULL;
	}
	if ( a->ob_size < size ) {
		size = a->ob_size;
		np->ob_size = size * n;
	}
	p = np->ob_item;
	for (i = 0; i < n; i++) {
		for (j = 0; j < size; j++) {
			*p = a->ob_item[j];
			INCREF(*p);
			p++;
		}
	}
	Py_LIST_UNLOCK(a);
	return (object *) np;
}

static int
list_ass_slice(a, ilow, ihigh, v)
	listobject *a;
	int ilow, ihigh;
	object *v;
{
	/* Because [X]DECREF can recursively invoke list operations on
	   this list, we must postpone all [X]DECREF activity until
	   after the list is back in its canonical shape.  Therefore
	   we must allocate an additional array, 'recycle', into which
	   we temporarily copy the items that are deleted from the
	   list. :-( */
	object **recycle, **p;
	object **item;
	int n; /* Size of replacement list */
	int d; /* Change in size */
	int k; /* Loop index */
#define b ((listobject *)v)
	if (v == NULL)
		n = 0;
	else if (is_listobject(v)) {
		n = b->ob_size;
		if (a == b) {
			/* Special case "a[i:j] = a" -- copy b first */
			int ret;
			v = list_slice(b, 0, n);
			ret = list_ass_slice(a, ilow, ihigh, v);
			DECREF(v);
			return ret;
		}
	}
	else {
		err_badarg();
		return -1;
	}
	Py_LIST_LOCK_TEST(a, -1);
	if ( b != NULL ) {
		if ( Py_LIST_LOCK(b) ) {
			Py_LIST_UNLOCK(a);
			return -1;
		}
		n = b->ob_size;
	}
	if (ilow < 0)
		ilow = 0;
	else if (ilow > a->ob_size)
		ilow = a->ob_size;
	if (ihigh < 0)
		ihigh = 0;
	if (ihigh < ilow)
		ihigh = ilow;
	else if (ihigh > a->ob_size)
		ihigh = a->ob_size;
	item = a->ob_item;
	d = n - (ihigh-ilow);
	if (ihigh > ilow)
		p = recycle = NEW(object *, (ihigh-ilow));
	else
		p = recycle = NULL;
	if (d <= 0) { /* Delete -d items; recycle ihigh-ilow items */
		for (k = ilow; k < ihigh; k++)
			*p++ = item[k];
		if (d < 0) {
			for (/*k = ihigh*/; k < a->ob_size; k++)
				item[k+d] = item[k];
			a->ob_size += d;
			NRESIZE(item, object *, a->ob_size); /* Can't fail */
			a->ob_item = item;
		}
	}
	else { /* Insert d items; recycle ihigh-ilow items */
		NRESIZE(item, object *, a->ob_size + d);
		if (item == NULL) {
			Py_LIST_UNLOCK(a);
			if ( b )
				Py_LIST_UNLOCK(b);
			XDEL(recycle);
			err_nomem();
			return -1;
		}
		for (k = a->ob_size; --k >= ihigh; )
			item[k+d] = item[k];
		for (/*k = ihigh-1*/; k >= ilow; --k)
			*p++ = item[k];
		a->ob_item = item;
		a->ob_size += d;
	}
	for (k = 0; k < n; k++, ilow++) {
		object *w = b->ob_item[k];
		XINCREF(w);
		item[ilow] = w;
	}
	Py_LIST_UNLOCK(a);
	if ( b )
		Py_LIST_UNLOCK(b);
	if (recycle) {
		while (--p >= recycle)
			XDECREF(*p);
		DEL(recycle);
	}
	return 0;
#undef b
}

int
setlistslice(a, ilow, ihigh, v)
	object *a;
	int ilow, ihigh;
	object *v;
{
	if (!is_listobject(a)) {
		err_badcall();
		return -1;
	}
	return list_ass_slice((listobject *)a, ilow, ihigh, v);
}

static int
list_ass_item(a, i, v)
	listobject *a;
	int i;
	object *v;
{
	object *old_value;
	if (i < 0 || i >= a->ob_size) {
		err_setstr(IndexError, "list assignment index out of range");
		return -1;
	}
	if (v == NULL)
		return list_ass_slice(a, i, i+1, v);
	Py_LIST_LOCK_TEST(a, -1);
	if (i >= a->ob_size) {
		Py_LIST_UNLOCK(a);
		err_setstr(IndexError, "list assignment index out of range");
		return -1;
	}
	INCREF(v);
	old_value = a->ob_item[i];
	a->ob_item[i] = v;
	Py_LIST_UNLOCK(a);
	DECREF(old_value); 
	return 0;
}

static object *
ins(self, where, v)
	listobject *self;
	int where;
	object *v;
{
	if (ins1(self, where, v) != 0)
		return NULL;
	INCREF(None);
	return None;
}

static object *
listinsert(self, args)
	listobject *self;
	object *args;
{
	int i;
	object *v;
	if (!getargs(args, "(iO)", &i, &v))
		return NULL;
	return ins(self, i, v);
}

static object *
listappend(self, args)
	listobject *self;
	object *args;
{
	object *v;
	if (!getargs(args, "O", &v))
		return NULL;
	return ins(self, (int) self->ob_size, v);
}

static int
cmp(v, w)
	const ANY *v, *w;
{
	object *t, *res;
	long i;
	PyThreadState *pts = PyThreadState_Get();


	if (err_occurred())
		return 0;

	if (pts->sort_comparefunc == NULL)
		return cmpobject(* (object **) v, * (object **) w);

	/* Call the user-supplied comparison function */
	t = mkvalue("(OO)", * (object **) v, * (object **) w);
	if (t == NULL)
		return 0;
	res = call_object(pts->sort_comparefunc, t);
	DECREF(t);
	if (res == NULL)
		return 0;
	if (!is_intobject(res)) {
		err_setstr(TypeError, "comparison function should return int");
		i = 0;
	}
	else {
		i = getintvalue(res);
		if (i < 0)
			i = -1;
		else if (i > 0)
			i = 1;
	}
	DECREF(res);
	return (int) i;
}

static object *
listsort(self, args)
	listobject *self;
	object *args;
{
	object *save_comparefunc;
	PyThreadState *pts = PyThreadState_Get();

	/* ### WARNING: IT IS "EASY" TO DEADLOCK WITH CUSTOM SORT FUNCS */

	Py_LIST_LOCK_TEST(self, NULL);
	if (self->ob_size <= 1) {
		Py_LIST_UNLOCK(self);
		INCREF(None);
		return None;
	}
	save_comparefunc = pts->sort_comparefunc;
	pts->sort_comparefunc = args;
	if (pts->sort_comparefunc != NULL) {
		/* Test the comparison function for obvious errors */
		(void) cmp((ANY *)&self->ob_item[0], (ANY *)&self->ob_item[1]);
		if (err_occurred()) {
			Py_LIST_UNLOCK(self);
			pts->sort_comparefunc = save_comparefunc;
			return NULL;
		}
	}
	qsort((char *)self->ob_item,
				(int) self->ob_size, sizeof(object *), cmp);
	Py_LIST_UNLOCK(self);
	pts->sort_comparefunc = save_comparefunc;
	if (err_occurred())
		return NULL;
	INCREF(None);
	return None;
}

static object *
listreverse(self, args)
	listobject *self;
	object *args;
{
	register object **p, **q;
	register object *tmp;
	
	if (args != NULL) {
		err_badarg();
		return NULL;
	}

	Py_LIST_LOCK_TEST(self, NULL);
	if (self->ob_size > 1) {
		for (p = self->ob_item, q = self->ob_item + self->ob_size - 1;
						p < q; p++, q--) {
			tmp = *p;
			*p = *q;
			*q = tmp;
		}
	}
	Py_LIST_UNLOCK(self);
	
	INCREF(None);
	return None;
}

int
reverselist(v)
	object *v;
{
	if (v == NULL || !is_listobject(v)) {
		err_badcall();
		return -1;
	}
	v = listreverse((listobject *)v, (object *)NULL);
	if (v == NULL)
		return -1;
	DECREF(v);
	return 0;
}

int
sortlist(v)
	object *v;
{
	if (v == NULL || !is_listobject(v)) {
		err_badcall();
		return -1;
	}
	v = listsort((listobject *)v, (object *)NULL);
	if (v == NULL)
		return -1;
	DECREF(v);
	return 0;
}

object *
listtuple(v)
	object *v;
{
	object *w;
	object **p;
	int n;
	if (v == NULL || !is_listobject(v)) {
		err_badcall();
		return NULL;
	}
	n = ((listobject *)v)->ob_size;
	w = newtupleobject(n);
	if (w == NULL)
		return NULL;
	if ( Py_LIST_LOCK(v) ) {
		Py_DECREF(w);
		return NULL;
	}
	if ( ((listobject *)v)->ob_size < n ) {
		n = ((listobject *)v)->ob_size;
		((tupleobject *)w)->ob_size = n;
	}
	p = ((tupleobject *)w)->ob_item;
	memcpy((ANY *)p,
	       (ANY *)((listobject *)v)->ob_item,
	       n*sizeof(object *));
	while (--n >= 0) {
		INCREF(*p);
		p++;
	}
	Py_LIST_UNLOCK(v);
	return w;
}

static object *
listindex(self, args)
	listobject *self;
	object *args;
{
	int i;
	
	if (args == NULL) {
		err_badarg();
		return NULL;
	}
#ifndef WITH_FREE_THREAD

	for (i = 0; i < self->ob_size; i++) {
		if (cmpobject(self->ob_item[i], args) == 0)
			return newintobject((long)i);
	}

#else

	for (i = 0; ; i++) {
		object *o;
		int rc;
		Py_LIST_LOCK_TEST(self, NULL);
		if ( i >= self->ob_size ) {
			Py_LIST_UNLOCK(self);
			break;
		}
		o = self->ob_item[i];
		Py_INCREF(o);
		Py_LIST_LAZY_UNLOCK(self);
		rc = cmpobject(o, args);
		Py_DECREF(o);
		if (rc == 0) {
			Py_LIST_LAZY_DONE(self);
			return newintobject((long)i);
		}
	}
	Py_LIST_LAZY_DONE(self);
#endif

	err_setstr(ValueError, "list.index(x): x not in list");
	return NULL;
}

static object *
listcount(self, args)
	listobject *self;
	object *args;
{
	int count = 0;
	int i;
	
	if (args == NULL) {
		err_badarg();
		return NULL;
	}

#ifndef WITH_FREE_THREAD

	for (i = 0; i < self->ob_size; i++) {
		if (cmpobject(self->ob_item[i], args) == 0)
			count++;
	}

#else

	for (i = 0; ; i++) {
		object *o;
		int rc;
		Py_LIST_LOCK_TEST(self, NULL);
		if ( i >= self->ob_size ) {
			Py_LIST_UNLOCK(self);
			break;
		}
		o = self->ob_item[i];
		Py_INCREF(o);
		Py_LIST_LAZY_UNLOCK(self);
		if ( cmpobject(o, args) == 0 )
			count++;
		Py_DECREF(o);
	}
	Py_LIST_LAZY_DONE(self);

#endif

	return newintobject((long)count);
}

static object *
listremove(self, args)
	listobject *self;
	object *args;
{
	int i;
	
	if (args == NULL) {
		err_badarg();
		return NULL;
	}

#ifndef WITH_FREE_THREAD

	for (i = 0; i < self->ob_size; i++) {
		if (cmpobject(self->ob_item[i], args) == 0) {
			if (list_ass_slice(self, i, i+1, (object *)NULL) != 0)
				return NULL;
			INCREF(None);
			return None;
		}
	}

#else

	for (i = 0; ; i++) {
		object *o;
		int rc;
		Py_LIST_LOCK_TEST(self, NULL);
		if ( i >= self->ob_size ) {
			Py_LIST_UNLOCK(self);
			break;
		}
		o = self->ob_item[i];
		Py_INCREF(o);
		Py_LIST_LAZY_UNLOCK(self);
		rc = cmpobject(o, args);
		Py_DECREF(o);
		if (rc == 0) {
			Py_LIST_LAZY_DONE(self);
			if (list_ass_slice(self, i, i+1, (object *)NULL) != 0)
				return NULL;
			INCREF(None);
			return None;
		}
	}
	Py_LIST_LAZY_DONE(self);
#endif

	err_setstr(ValueError, "list.remove(x): x not in list");
	return NULL;
}

static struct methodlist list_methods[] = {
	{"append",	(method)listappend},
	{"count",	(method)listcount},
	{"index",	(method)listindex},
	{"insert",	(method)listinsert},
	{"sort",	(method)listsort, 0},
	{"remove",	(method)listremove},
	{"reverse",	(method)listreverse},
	{NULL,		NULL}		/* sentinel */
};

static object *
list_getattr(f, name)
	listobject *f;
	char *name;
{
	return findmethod(list_methods, (object *)f, name);
}

static sequence_methods list_as_sequence = {
	(inquiry)list_length, /*sq_length*/
	(binaryfunc)list_concat, /*sq_concat*/
	(intargfunc)list_repeat, /*sq_repeat*/
	(intargfunc)list_item, /*sq_item*/
	(intintargfunc)list_slice, /*sq_slice*/
	(intobjargproc)list_ass_item, /*sq_ass_item*/
	(intintobjargproc)list_ass_slice, /*sq_ass_slice*/
};

typeobject Listtype = {
	OB_HEAD_INIT(&Typetype)
	0,
	"list",
	sizeof(listobject),
	0,
	(destructor)list_dealloc, /*tp_dealloc*/
	(printfunc)list_print, /*tp_print*/
	(getattrfunc)list_getattr, /*tp_getattr*/
	0,		/*tp_setattr*/
	(cmpfunc)list_compare, /*tp_compare*/
	(reprfunc)list_repr, /*tp_repr*/
	0,		/*tp_as_number*/
	&list_as_sequence,	/*tp_as_sequence*/
	0,		/*tp_as_mapping*/
};
