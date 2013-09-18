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

/* Mapping object implementation; using a hash table */

/* This file should really be called "dictobject.c", since "mapping"
  is the generic name for objects with an unorderred arbitrary key
  set (just like lists are sequences), but since it improves (and was
  originally derived from) a file by that name I had to change its
  name.  For the user these objects are still called "dictionaries". */

#include "allobjects.h"
#include "modsupport.h"
#include "pymutex.h"
#include "pypooledlock.h"

/*
Table of primes suitable as keys, in ascending order.
The first line are the largest primes less than some powers of two,
the second line is the largest prime less than 6000,
the third line is a selection from Knuth, Vol. 3, Sec. 6.1, Table 1,
and the next three lines were suggested by Steve Kirsch.
The final value is a sentinel.
*/
static long primes[] = {
	3, 7, 13, 31, 61, 127, 251, 509, 1021, 2017, 4093,
	5987,
	9551, 15683, 19609, 31397,
	65521L, 131071L, 262139L, 524287L, 1048573L, 2097143L,
	4194301L, 8388593L, 16777213L, 33554393L, 67108859L,
	134217689L, 268435399L, 536870909L, 1073741789L,
	0
};

/* Object used as dummy key to fill deleted entries */
static object *dummy; /* Initialized by first call to newmappingobject() */

/*
Invariant for entries: when in use, de_value is not NULL and de_key is
not NULL and not dummy; when not in use, de_value is NULL and de_key
is either NULL or dummy.  A dummy key value cannot be replaced by
NULL, since otherwise other keys may be lost.
*/
typedef struct {
	long me_hash;
	object *me_key;
	object *me_value;
} mappingentry;

/*
To ensure the lookup algorithm terminates, the table size must be a
prime number and there must be at least one NULL key in the table.
The value ma_fill is the number of non-NULL keys; ma_used is the number
of non-NULL, non-dummy keys.
To avoid slowing down lookups on a near-full table, we resize the table
when it is more than half filled.
*/
typedef struct {
	OB_HEAD
	Py_DECLARE_POOLED_LOCK
	int ma_fill;
	int ma_used;
	int ma_size;
	mappingentry *ma_table;
} mappingobject;

#define Py_MAP_LOCK(mp)		Py_POOLED_LOCK((mappingobject *)(mp))
#define Py_MAP_LOCK_TEST(mp,v)	Py_POOLED_LOCK_TEST((mappingobject *)(mp), (v))
#define Py_MAP_UNLOCK(mp)	Py_POOLED_UNLOCK((mappingobject *)(mp))
#define Py_MAP_LAZY_UNLOCK(mp)	Py_POOLED_LAZY_UNLOCK((mappingobject *)(mp))
#define Py_MAP_LAZY_DONE(mp)	Py_POOLED_LAZY_DONE((mappingobject *)(mp))

object *
newmappingobject()
{
	register mappingobject *mp;
	if (dummy == NULL) { /* Auto-initialize dummy */
		dummy = newstringobject("<dummy key>");
		if (dummy == NULL)
			return NULL;
	}
	mp = NEWOBJ(mappingobject, &Mappingtype);
	if (mp == NULL)
		return NULL;
	mp->ma_size = 0;
	mp->ma_table = NULL;
	mp->ma_fill = 0;
	mp->ma_used = 0;
	Py_POOLED_INIT(mp);
	return (object *)mp;
}

/*
The basic lookup function used by all operations.
This is essentially Algorithm D from Knuth Vol. 3, Sec. 6.4.
Open addressing is preferred over chaining since the link overhead for
chaining would be substantial (100% with typical malloc overhead).

First a 32-bit hash value, 'sum', is computed from the key string.
The first character is added an extra time shifted by 8 to avoid hashing
single-character keys (often heavily used variables) too close together.
All arithmetic on sum should ignore overflow.

The initial probe index is then computed as sum mod the table size.
Subsequent probe indices are incr apart (mod table size), where incr
is also derived from sum, with the additional requirement that it is
relative prime to the table size (i.e., 1 <= incr < size, since the size
is a prime number).  My choice for incr is somewhat arbitrary.
*/
/*
** NOTE: we assume the mapping is locked on entry and exit (it must be
** locked on exit so that the mappingentry remains valid).  To simplify
** the whole system, we do NOT unlock the mapping when cmpobject() is
** called.
**
** THEREFORE, if the key objects in a mapping have a __cmp__ method that
** somehow refers back to the mapping, then a DEADLOCK may occur.
**
** NOTE: even in a non-threaded environment, if a __cmp__ method changes
** the mapping, corruption may occur.  In a threaded environment, the
** additional restriction of not being able to refer to the mapping
** is also applied.
*/
static mappingentry *lookmapping PROTO((mappingobject *, object *, long));
static mappingentry *
lookmapping(mp, key, hash)
	register mappingobject *mp;
	object *key;
	long hash;
{
	register int i, incr;
	register unsigned long sum = (unsigned long) hash;
	register mappingentry *freeslot = NULL;
	register int size = mp->ma_size;
	/* We must come up with (i, incr) such that 0 <= i < ma_size
	   and 0 < incr < ma_size and both are a function of hash */
	i = sum % size;
	do {
		sum = 3*sum + 1;
		incr = sum % size;
	} while (incr == 0);
	for (;;) {
		register mappingentry *ep = &mp->ma_table[i];
		if (ep->me_key == NULL) {
			if (freeslot != NULL)
				return freeslot;
			else
				return ep;
		}
		if (ep->me_key == dummy) {
			if (freeslot == NULL)
				freeslot = ep;
		}
		else if (ep->me_hash == hash &&
			 cmpobject(ep->me_key, key) == 0) {
			return ep;
		}
		i = (i + incr) % size;
	}
}

/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Eats a reference to key and one to value.
*/
#ifdef WITH_FREE_THREAD
static void insertmapping PROTO((mappingobject *, object *, long, object *, int));
#else
static void insertmapping PROTO((mappingobject *, object *, long, object *));
#endif
static void
insertmapping(mp, key, hash, value
#ifdef WITH_FREE_THREAD
	      , handle_locks
#endif
    )
	register mappingobject *mp;
	object *key;
	long hash;
	object *value;
#ifdef WITH_FREE_THREAD
	int handle_locks;
#endif
{
	object *old_value;
	register mappingentry *ep;
	ep = lookmapping(mp, key, hash);
	if (ep->me_value != NULL) {
		old_value = ep->me_value;
		ep->me_value = value;
#ifdef WITH_FREE_THREAD
		if ( handle_locks )
			Py_MAP_UNLOCK(mp);
#endif
		DECREF(old_value); /* which **CAN** re-enter */
		DECREF(key);
	}
	else {
		old_value = ep->me_key;
		if (old_value == NULL)
			mp->ma_fill++;
		ep->me_key = key;
		ep->me_hash = hash;
		ep->me_value = value;
		mp->ma_used++;
#ifdef WITH_FREE_THREAD
		if ( handle_locks )
			Py_MAP_UNLOCK(mp);
#endif
		XDECREF(old_value);
	}
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
*/
static int mappingresize PROTO((mappingobject *));
static int
mappingresize(mp)
	mappingobject *mp;
{
	register int oldsize = mp->ma_size;
	register int newsize;
	register mappingentry *oldtable = mp->ma_table;
	register mappingentry *newtable;
	register mappingentry *ep;
	register int i;

	Py_MAP_LOCK_TEST(mp, -1);
	newsize = mp->ma_size;
	for (i = 0; ; i++) {
		if (primes[i] <= 0) {
			/* Ran out of primes */
			Py_MAP_UNLOCK(mp);
			err_nomem();
			return -1;
		}
		if (primes[i] > mp->ma_used*2) {
			newsize = primes[i];
			if (newsize != primes[i]) {
				/* Integer truncation */
				Py_MAP_UNLOCK(mp);
				err_nomem();
				return -1;
			}
			break;
		}
	}
	newtable = (mappingentry *) calloc(sizeof(mappingentry), newsize);
	if (newtable == NULL) {
		Py_MAP_UNLOCK(mp);
		err_nomem();
		return -1;
	}
	mp->ma_size = newsize;
	mp->ma_table = newtable;
	mp->ma_fill = 0;
	mp->ma_used = 0;

	/* Make two passes, so we can avoid decrefs
	   (and possible side effects) till the table is copied */
	for (i = 0, ep = oldtable; i < oldsize; i++, ep++) {
		if (ep->me_value != NULL)
			insertmapping(mp,ep->me_key,ep->me_hash,ep->me_value
#ifdef WITH_FREE_THREAD
				      , 0	/* NO lock management */
#endif
				      );
	}
	Py_MAP_UNLOCK(mp);
	for (i = 0, ep = oldtable; i < oldsize; i++, ep++) {
		if (ep->me_value == NULL)
			XDECREF(ep->me_key);
	}

	XDEL(oldtable);
	return 0;
}

object *
mappinglookup(op, key)
	object *op;
	object *key;
{
	long hash;
	if (!is_mappingobject(op)) {
		err_badcall();
		return NULL;
	}
	if (((mappingobject *)op)->ma_table == NULL)
		return NULL;
#ifdef CACHE_HASH
	if (!is_stringobject(key) || (hash = ((stringobject *) key)->ob_shash) == -1)
#endif
	hash = hashobject(key);
	if (hash == -1)
		return NULL;
#ifndef WITH_FREE_THREAD
	return lookmapping((mappingobject *)op, key, hash) -> me_value;
#else
	{
		object *value;
		Py_MAP_LOCK_TEST(op, NULL);
		value = lookmapping((mappingobject *)op, key, hash) -> me_value;
		Py_MAP_UNLOCK(op);
		return value;
	}
#endif
}

int
mappinginsert(op, key, value)
	register object *op;
	object *key;
	object *value;
{
	register mappingobject *mp;
	register long hash;
	if (!is_mappingobject(op)) {
		err_badcall();
		return -1;
	}
#ifdef CACHE_HASH
	if (!is_stringobject(key) || (hash = ((stringobject *) key)->ob_shash) == -1)
#endif
	hash = hashobject(key);
	if (hash == -1)
		return -1;
	mp = (mappingobject *)op;
	/* if fill >= 2/3 size, resize */
	if (mp->ma_fill*3 >= mp->ma_size*2) {
		if (mappingresize(mp) != 0) {
			if (mp->ma_fill+1 > mp->ma_size)
				return -1;
		}
	}
	Py_MAP_LOCK_TEST(mp, -1);
	INCREF(value);
	INCREF(key);
	insertmapping(mp, key, hash, value
#ifdef WITH_FREE_THREAD
		      , 1	/* do lock management */
#endif
	    );
	return 0;
}

int
mappingremove(op, key)
	object *op;
	object *key;
{
	register mappingobject *mp;
	register long hash;
	register mappingentry *ep;
	object *old_value, *old_key;

	if (!is_mappingobject(op)) {
		err_badcall();
		return -1;
	}
#ifdef CACHE_HASH
	if (!is_stringobject(key) || (hash = ((stringobject *) key)->ob_shash) == -1)
#endif
	hash = hashobject(key);
	if (hash == -1)
		return -1;
	mp = (mappingobject *)op;
	if (((mappingobject *)op)->ma_table == NULL)
		goto empty;
	Py_MAP_LOCK_TEST(mp, -1);
	ep = lookmapping(mp, key, hash);
	if (ep->me_value == NULL) {
		Py_MAP_UNLOCK(mp);
	empty:
		err_setval(KeyError, key);
		return -1;
	}
	old_key = ep->me_key;
	INCREF(dummy);
	ep->me_key = dummy;
	old_value = ep->me_value;
	ep->me_value = NULL;
	mp->ma_used--;
	Py_MAP_UNLOCK(mp);
	DECREF(old_value); 
	DECREF(old_key); 
	return 0;
}

void
mappingclear(op)
	object *op;
{
	int i, n;
	register mappingentry *table;
	mappingobject *mp;
	if (!is_mappingobject(op))
		return;
	mp = (mappingobject *)op;
	if ( Py_MAP_LOCK(mp) )
		return;
	table = mp->ma_table;
	if (table == NULL) {
		Py_MAP_UNLOCK(mp);
		return;
	}
	n = mp->ma_size;
	mp->ma_size = mp->ma_used = mp->ma_fill = 0;
	mp->ma_table = NULL;
	Py_MAP_UNLOCK(mp);
	for (i = 0; i < n; i++) {
		XDECREF(table[i].me_key);
		XDECREF(table[i].me_value);
	}
	DEL(table);
}

/* WARNING: not thread-safe since it does not INCREF return values */
int
mappinggetnext(op, ppos, pkey, pvalue)
	object *op;
	int *ppos;
	object **pkey;
	object **pvalue;
{
	int i;
	register mappingobject *mp;
	if (!is_dictobject(op))
		return 0;
	mp = (mappingobject *)op;
	i = *ppos;
	if (i < 0)
		return 0;
	while (i < mp->ma_size && mp->ma_table[i].me_value == NULL)
		i++;
	*ppos = i+1;
	if (i >= mp->ma_size)
		return 0;
	if (pkey)
		*pkey = mp->ma_table[i].me_key;
	if (pvalue)
		*pvalue = mp->ma_table[i].me_value;
	return 1;
}

/* Methods */

static void
mapping_dealloc(mp)
	register mappingobject *mp;
{
	register int i;
	register mappingentry *ep;
	for (i = 0, ep = mp->ma_table; i < mp->ma_size; i++, ep++) {
		if (ep->me_key != NULL)
			DECREF(ep->me_key);
		if (ep->me_value != NULL)
			DECREF(ep->me_value);
	}
	XDEL(mp->ma_table);
	Py_POOLED_LAZY_DONE(mp);
	DEL(mp);
}

static int
mapping_print(mp, fp, flags)
	register mappingobject *mp;
	register FILE *fp;
	register int flags;
{
	register int i;
	register int any;
	register mappingentry *ep;
	fprintf(fp, "{");
	any = 0;
#ifndef WITH_FREE_THREAD
	for (i = 0, ep = mp->ma_table; i < mp->ma_size; i++, ep++) {
		if (ep->me_value != NULL) {
			if (any++ > 0)
				fprintf(fp, ", ");
			if (printobject((object *)ep->me_key, fp, 0) != 0)
				return -1;
			fprintf(fp, ": ");
			if (printobject(ep->me_value, fp, 0) != 0)
				return -1;
		}
	}
#else
	Py_MAP_LOCK_TEST(mp, -1);
	for (i = 0, ep = mp->ma_table; i < mp->ma_size; i++, ep++) {
		if (ep->me_value != NULL) {
			object *key;
			object *value;
			int rc;
			if (any++ > 0)
				fprintf(fp, ", ");
			key = ep->me_key;
			value = ep->me_value;
			INCREF(key);
			INCREF(value);
			Py_MAP_LAZY_UNLOCK(mp);
			rc = printobject(key, fp, 0);
			DECREF(key);
			if (rc == 0) {
				fprintf(fp, ": ");
				rc = printobject(value, fp, 0);
			}
			DECREF(value);
			if (rc != 0) {
				Py_MAP_LAZY_DONE(mp);
				return -1;
			}
			Py_MAP_LOCK_TEST(mp, -1);
			/* re-establish the pointer */
			ep = &mp->ma_table[i];
		}
	}
	Py_MAP_UNLOCK(mp);
#endif
	fprintf(fp, "}");
	return 0;
}

static object *
mapping_repr(mp)
	mappingobject *mp;
{
	auto object *v;
	object *sepa, *colon;
	register int i;
	register int any;
	register mappingentry *ep;
	v = newstringobject("{");
	sepa = newstringobject(", ");
	colon = newstringobject(": ");
	any = 0;
#ifndef WITH_FREE_THREAD
	for (i = 0, ep = mp->ma_table; i < mp->ma_size && v; i++, ep++) {
		if (ep->me_value != NULL) {
			if (any++)
				joinstring(&v, sepa);
			joinstring_decref(&v, reprobject(ep->me_key));
			joinstring(&v, colon);
			joinstring_decref(&v, reprobject(ep->me_value));
		}
	}
#else
	if ( Py_MAP_LOCK(mp) ) {
		XDECREF(v);
		XDECREF(sepa);
		XDECREF(colon);
		return NULL;
	}
	for (i = 0, ep = mp->ma_table; i < mp->ma_size && v; i++, ep++) {
		if (ep->me_value != NULL) {
			object *key;
			object *value;
			key = ep->me_key;
			value = ep->me_value;
			INCREF(key);
			INCREF(value);
			Py_MAP_LAZY_UNLOCK(mp);

			if (any++)
				joinstring(&v, sepa);
			joinstring_decref(&v, reprobject(key));
			DECREF(key);
			joinstring(&v, colon);
			joinstring_decref(&v, reprobject(value));
			DECREF(value);
			if ( Py_MAP_LOCK(mp) ) {
				XDECREF(v);
				XDECREF(sepa);
				XDECREF(colon);
				return NULL;
			}
			/* re-establish the pointer */
			ep = &mp->ma_table[i];
		}
	}
	Py_MAP_UNLOCK(mp);
#endif
	joinstring_decref(&v, newstringobject("}"));
	XDECREF(sepa);
	XDECREF(colon);
	return v;
}

static int
mapping_length(mp)
	mappingobject *mp;
{
	return mp->ma_used;
}

static object *
mapping_subscript(mp, key)
	mappingobject *mp;
	register object *key;
{
	object *v;
	long hash;
	if (mp->ma_table == NULL) {
		err_setval(KeyError, key);
		return NULL;
	}
#ifdef CACHE_HASH
	if (!is_stringobject(key) || (hash = ((stringobject *) key)->ob_shash) == -1)
#endif
	hash = hashobject(key);
	if (hash == -1)
		return NULL;
	Py_MAP_LOCK_TEST(mp, NULL);
	v = lookmapping(mp, key, hash) -> me_value;
	if (v == NULL) {
		Py_MAP_UNLOCK(mp);
		err_setval(KeyError, key);
	} else {
		INCREF(v);
		Py_MAP_UNLOCK(mp);
	}
	return v;
}

static int
mapping_ass_sub(mp, v, w)
	mappingobject *mp;
	object *v, *w;
{
	if (w == NULL)
		return mappingremove((object *)mp, v);
	else
		return mappinginsert((object *)mp, v, w);
}

static mapping_methods mapping_as_mapping = {
	(inquiry)mapping_length, /*mp_length*/
	(binaryfunc)mapping_subscript, /*mp_subscript*/
	(objobjargproc)mapping_ass_sub, /*mp_ass_subscript*/
};

static object *
mapping_keys(mp, args)
	register mappingobject *mp;
	object *args;
{
	register object *v;
	register int i, j;
	if (!getnoarg(args))
		return NULL;
	v = newlistobject(mp->ma_used);
	if (v == NULL)
		return NULL;

	/* NOTE: we leave the mapping locked the whole time... this could
	   theoretically deadlock in the weirdest of situations */
	if ( Py_MAP_LOCK(mp) ) {
		DECREF(v);
		return NULL;
	}
	for (i = 0, j = 0; i < mp->ma_size; i++) {
		if (mp->ma_table[i].me_value != NULL) {
			object *key = mp->ma_table[i].me_key;
			INCREF(key);
			setlistitem(v, j, key);
			j++;
		}
	}
	Py_MAP_UNLOCK(mp);
	return v;
}

static object *
mapping_values(mp, args)
	register mappingobject *mp;
	object *args;
{
	register object *v;
	register int i, j;
	if (!getnoarg(args))
		return NULL;
	v = newlistobject(mp->ma_used);
	if (v == NULL)
		return NULL;

	/* NOTE: we leave the mapping locked the whole time... this could
	   theoretically deadlock in the weirdest of situations */
	if ( Py_MAP_LOCK(mp) ) {
		DECREF(v);
		return NULL;
	}
	for (i = 0, j = 0; i < mp->ma_size; i++) {
		if (mp->ma_table[i].me_value != NULL) {
			object *value = mp->ma_table[i].me_value;
			INCREF(value);
			setlistitem(v, j, value);
			j++;
		}
	}
	Py_MAP_UNLOCK(mp);
	return v;
}

static object *
mapping_items(mp, args)
	register mappingobject *mp;
	object *args;
{
	register object *v;
	register int i, j;
	if (!getnoarg(args))
		return NULL;
	v = newlistobject(mp->ma_used);
	if (v == NULL)
		return NULL;

	/* NOTE: we leave the mapping locked the whole time... this could
	   theoretically deadlock in the weirdest of situations */
	if ( Py_MAP_LOCK(mp) ) {
		DECREF(v);
		return NULL;
	}
	for (i = 0, j = 0; i < mp->ma_size; i++) {
		if (mp->ma_table[i].me_value != NULL) {
			object *key = mp->ma_table[i].me_key;
			object *value = mp->ma_table[i].me_value;
			object *item = newtupleobject(2);
			if (item == NULL) {
				Py_MAP_UNLOCK(mp);
				DECREF(v);
				return NULL;
			}
			INCREF(key);
			settupleitem(item, 0, key);
			INCREF(value);
			settupleitem(item, 1, value);
			setlistitem(v, j, item);
			j++;
		}
	}
	Py_MAP_UNLOCK(mp);
	return v;
}

int
getmappingsize(mp)
	object *mp;
{
	if (mp == NULL || !is_mappingobject(mp)) {
		err_badcall();
		return 0;
	}
	return ((mappingobject *)mp)->ma_used;
}

object *
getmappingkeys(mp)
	object *mp;
{
	if (mp == NULL || !is_mappingobject(mp)) {
		err_badcall();
		return NULL;
	}
	return mapping_keys((mappingobject *)mp, (object *)NULL);
}

object *
getmappingvalues(mp)
	object *mp;
{
	if (mp == NULL || !is_mappingobject(mp)) {
		err_badcall();
		return NULL;
	}
	return mapping_values((mappingobject *)mp, (object *)NULL);
}

object *
getmappingitems(mp)
	object *mp;
{
	if (mp == NULL || !is_mappingobject(mp)) {
		err_badcall();
		return NULL;
	}
	return mapping_items((mappingobject *)mp, (object *)NULL);
}

static int
mapping_compare(a, b)
	mappingobject *a, *b;
{
	object *akeys, *bkeys;
	int i, n, res;
	if (a == b)
		return 0;
	if (a->ma_used == 0) {
		if (b->ma_used != 0)
			return -1;
		else
			return 0;
	}
	else {
		if (b->ma_used == 0)
			return 1;
	}
	akeys = mapping_keys(a, (object *)NULL);
	bkeys = mapping_keys(b, (object *)NULL);
	if (akeys == NULL || bkeys == NULL) {
		/* Oops, out of memory -- what to do? */
		/* For now, sort on address! */
		XDECREF(akeys);
		XDECREF(bkeys);
		if (a < b)
			return -1;
		else
			return 1;
	}
	sortlist(akeys);
	sortlist(bkeys);
	/* get the smallest */
	n = ((listobject *)akeys)->ob_size < ((listobject *)bkeys)->ob_size ?
	    ((listobject *)akeys)->ob_size : ((listobject *)bkeys)->ob_size;
	res = 0;
	for (i = 0; i < n; i++) {
		object *akey, *bkey, *aval, *bval;
		long ahash, bhash;
		akey = getlistitem(akeys, i);
		bkey = getlistitem(bkeys, i);
		res = cmpobject(akey, bkey);
		if (res != 0)
			break;
#ifdef CACHE_HASH
		if (!is_stringobject(akey) || (ahash = ((stringobject *) akey)->ob_shash) == -1)
#endif
		ahash = hashobject(akey);
		if (ahash == -1)
			err_clear(); /* Don't want errors here */
#ifdef CACHE_HASH
		if (!is_stringobject(bkey) || (bhash = ((stringobject *) bkey)->ob_shash) == -1)
#endif
		bhash = hashobject(bkey);
		if (bhash == -1)
			err_clear(); /* Don't want errors here */
		if ( Py_MAP_LOCK(a) ) {
			err_clear(); /* Don't want errors here */
			break;
		}
		if ( Py_MAP_LOCK(b) ) {
			err_clear(); /* Don't want errors here */
			Py_MAP_UNLOCK(a);
			break;
		}
		aval = lookmapping(a, akey, ahash) -> me_value;
		bval = lookmapping(b, bkey, bhash) -> me_value;
#ifdef WITH_FREE_THREAD
		XINCREF(aval);
		XINCREF(bval);
		Py_MAP_LAZY_UNLOCK(a);
		Py_MAP_LAZY_UNLOCK(b);
		if ( aval == NULL || bval == NULL ) {
			/* a key/value pair disappeared; we're done */
			XDECREF(aval);
			XDECREF(bval);
			break;
		}
#endif
		res = cmpobject(aval, bval);
#ifdef WITH_FREE_THREAD
		DECREF(aval);
		DECREF(bval);
#endif
		if (res != 0)
			break;
	}
	if (res == 0) {
		if (a->ma_used < b->ma_used)
			res = -1;
		else if (a->ma_used > b->ma_used)
			res = 1;
	}
	DECREF(akeys);
	DECREF(bkeys);
	Py_MAP_LAZY_DONE(a);
	Py_MAP_LAZY_DONE(b);
	return res;
}

static object *
mapping_has_key(mp, args)
	register mappingobject *mp;
	object *args;
{
	object *key;
	long hash;
	register long ok;
	if (!getargs(args, "O", &key))
		return NULL;
#ifdef CACHE_HASH
	if (!is_stringobject(key) || (hash = ((stringobject *) key)->ob_shash) == -1)
#endif
	hash = hashobject(key);
	if (hash == -1)
		return NULL;
	Py_MAP_LOCK_TEST(mp, NULL);
	ok = mp->ma_size != 0 && lookmapping(mp, key, hash)->me_value != NULL;
	Py_MAP_UNLOCK(mp);
	return newintobject(ok);
}

static struct methodlist mapp_methods[] = {
	{"has_key",	(method)mapping_has_key},
	{"items",	(method)mapping_items},
	{"keys",	(method)mapping_keys},
	{"values",	(method)mapping_values},
	{NULL,		NULL}		/* sentinel */
};

static object *
mapping_getattr(mp, name)
	mappingobject *mp;
	char *name;
{
	return findmethod(mapp_methods, (object *)mp, name);
}

typeobject Mappingtype = {
	OB_HEAD_INIT(&Typetype)
	0,
	"dictionary",
	sizeof(mappingobject),
	0,
	(destructor)mapping_dealloc, /*tp_dealloc*/
	(printfunc)mapping_print, /*tp_print*/
	(getattrfunc)mapping_getattr, /*tp_getattr*/
	0,			/*tp_setattr*/
	(cmpfunc)mapping_compare, /*tp_compare*/
	(reprfunc)mapping_repr, /*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	&mapping_as_mapping,	/*tp_as_mapping*/
};

/* For backward compatibility with old dictionary interface */

static object *volatile last_name_object;
static char *volatile last_name_char; /* NULL or == getstringvalue(last_name_object) */

object *
getattro(v, name)
	object *v;
	object *name;
{
	char *attr;

	if (v->ob_type->tp_getattro != NULL)
		return (*v->ob_type->tp_getattro)(v, name);

#ifdef POSSIBLE_OPTIMIZATION
	/* Note that 'name' is ref'd and that its string has longer scope
	   than this function (implying last_name_object may have longer
	   scope, too, meaning we can simply use the pointer in
	   last_name_char, even if last_name_* subsequently change).
	   This approach may also optimize future uses. */

	Py_CRIT_LOCK();
	if (name == last_name_object) {
		attr = last_name_char;
		Py_CRIT_UNLOCK();
	}
	else {
		object *temp;

		Py_CRIT_UNLOCK();

		INCREF(name);	/* for storing in last_name_object */
		attr = getstringvalue(name);

		Py_CRIT_LOCK();
		temp = last_name_object;
		last_name_object = name;
		last_name_char = attr;
		Py_CRIT_UNLOCK();

		XDECREF(temp);
	}
#else
	attr = getstringvalue(name);
	if ( !attr )
		return NULL;
#endif

	return getattr(v, attr);
}

int
setattro(v, name, value)
	object *v;
	object *name;
	object *value;
{
	char *attr;

	if (v->ob_type->tp_setattro != NULL)
		return (*v->ob_type->tp_setattro)(v, name, value);

#ifdef POSSIBLE_OPTIMIZATION
	/* Note that 'name' is ref'd and that its string has longer scope
	   than this function (implying last_name_object may have longer
	   scope, too, meaning we can simply use the pointer in
	   last_name_char, even if last_name_* subsequently change).
	   This approach may also optimize future uses. */

	Py_CRIT_LOCK();
	if (name == last_name_object) {
		attr = last_name_char;
		Py_CRIT_UNLOCK();
	}
	else {
		object *temp;

		Py_CRIT_UNLOCK();

		INCREF(name);	/* for storing in last_name_object */
		attr = getstringvalue(name);

		Py_CRIT_LOCK();
		temp = last_name_object;
		last_name_object = name;
		last_name_char = attr;
		Py_CRIT_UNLOCK();

		XDECREF(temp);
	}
#else
	attr = getstringvalue(name);
	if ( !attr )
		return -1;
#endif

	return setattr(v, attr, value);
}


/*
** NOTE: the routines below would not benefit from using last_name_object
** and last_name_char.  There is no way to tell, based solely on the "key"
** variable, whether the value of the key is the same as what was placed
** into last_name_*.
*/

object *
dictlookup(v, key)
	object *v;
	char *key;
{
	object *result;
	object *keyo = newstringobject(key);
	if ( keyo == NULL )
		return NULL;
	result = mappinglookup(v, keyo);
	Py_DECREF(keyo);
	return result;
}

int
dictinsert(v, key, item)
	object *v;
	char *key;
	object *item;
{
	int result;
	object *keyo = newstringobject(key);
	if ( keyo == NULL )
		return -1;

	result = mappinginsert(v, keyo, item);
	Py_DECREF(keyo);
	return result;
}

int
dictremove(v, key)
	object *v;
	char *key;
{
	int result;
	object *keyo = newstringobject(key);
	if ( keyo == NULL )
		return -1;

	result = mappingremove(v, keyo);
	Py_DECREF(keyo);
	return result;
}
