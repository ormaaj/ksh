/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1985-2012 AT&T Intellectual Property          *
*          Copyright (c) 2020-2025 Contributors to ksh 93u+m           *
*                      and is licensed under the                       *
*                 Eclipse Public License, Version 2.0                  *
*                                                                      *
*                A copy of the License is available at                 *
*      https://www.eclipse.org/org/documents/epl-2.0/EPL-2.0.html      *
*         (with md5 checksum 84283fa8859daf213bdda5a9f8d1be1d)         *
*                                                                      *
*                 Glenn Fowler <gsf@research.att.com>                  *
*                  David Korn <dgk@research.att.com>                   *
*                   Phong Vo <kpv@research.att.com>                    *
*                  Martijn Dekker <martijn@inlv.org>                   *
*            Johnothan King <johnothanking@protonmail.com>             *
*                                                                      *
***********************************************************************/
#include	"dthdr.h"

/*	List, Deque, Stack, Queue.
**
**	Written by Kiem-Phong Vo (05/25/96)
*/

typedef struct _dtlist_s
{	Dtdata_t	data;
	Dtlink_t*	link;	/* list of objects		*/
	Dtlink_t*	here;	/* finger to searched objects	*/
} Dtlist_t;

#ifdef DEBUG
int dtlistprint(Dt_t* dt, Dtlink_t* here, char* (*objprintf)(void*) )
{
	int		k;
	char		*obj, *endb, buf[1024];
	Dtdisc_t	*disc = dt->disc;
	Dtlist_t	*list = (Dtlist_t*)dt->data;

	if(!here && !(here = list->link) )
		return -1;

	for(; here; here = here->_rght)
	{	endb = buf; /* indentation */
		*endb++ = '(';
		obj = (*objprintf)(_DTOBJ(disc, here));
		k = strlen(obj); memcpy(endb, obj, k); endb += k;
		*endb++ = ')';
		*endb++ = '\n';
		write(2, buf, endb-buf);
	}

	return 0;
}
#endif

/* terminal objects: DT_FIRST|DT_LAST */
void* lfirstlast(Dt_t* dt, int type)
{
	Dtlink_t	*lnk;
	Dtdisc_t	*disc = dt->disc;
	Dtlist_t	*list = (Dtlist_t*)dt->data;

	if((lnk = list->link) )
	{	if(type&DT_LAST)
			lnk = lnk->_left;
		list->here = lnk; /* finger points to this */
	}

	return lnk ? _DTOBJ(disc,lnk) : NULL;
}

/* DT_CLEAR */
void* lclear(Dt_t* dt)
{
	Dtlink_t	*lnk, *next;
	Dtdisc_t	*disc = dt->disc;
	Dtlist_t	*list = (Dtlist_t*)dt->data;

	lnk = list->link;
	list->link = list->here = NULL;
	list->data.size = 0;

	if(disc->freef || disc->link < 0)
	{	for(; lnk; lnk = next)
		{	next = lnk->_rght;
			_dtfree(dt, lnk, DT_DELETE);
		}
	}

	return NULL;
}

/* DT_FLATTEN|DT_EXTRACT|DT_RESTORE */
void* llist(Dt_t* dt, Dtlink_t* lnk, int type)
{
	Dtlist_t	*list = (Dtlist_t*)dt->data;

	if(type&(DT_FLATTEN|DT_EXTRACT) )
	{	if(lnk) /* error on calling */
			return NULL;

		lnk = list->link;
		if(type&DT_EXTRACT)
		{	list->link = NULL;
			dt->data->size = 0;
		}
	}
	else /* if(type&DT_RESTORE) */
	{	if(list->link != NULL)
			return NULL;

		list->link = lnk;

		dt->data->size = 0;
		for(; lnk; lnk = lnk->_rght)
			dt->data->size += 1;
	}

	return lnk;
}

static void* listat(Dt_t* dt, Dtstat_t* st)
{
	if(st)
	{	memset(st, 0, sizeof(Dtstat_t));
		st->meth  = dt->meth->type;
		st->size  = dt->data->size;
		st->space = sizeof(Dtlist_t) + (dt->disc->link >= 0 ? 0 : dt->data->size*sizeof(Dthold_t));
	}

	return (void*)dt->data->size;
}

static void* dtlist(Dt_t* dt, void* obj, int type)
{
	Dtlink_t	*r, *t, *h;
	void		*key, *o, *k;
	Dtdisc_t	*disc = dt->disc;
	Dtlist_t	*list = (Dtlist_t*)dt->data;

	type = DTTYPE(dt,type); /* map type for upward compatibility */
	if(!(type&DT_OPERATIONS) )
		return NULL;

	DTSETLOCK(dt);

	if(type&(DT_FIRST|DT_LAST) )
		DTRETURN(obj, lfirstlast(dt, type));
	else if(type&(DT_EXTRACT|DT_RESTORE|DT_FLATTEN) )
		DTRETURN(obj, llist(dt, (Dtlink_t*)obj, type));
	else if(type&DT_CLEAR)
		DTRETURN(obj, lclear(dt));
	else if(type&DT_STAT )
		DTRETURN(obj, listat(dt, (Dtstat_t*)obj));

	h = list->here; /* save finger to last search object */
	list->here = NULL;

	if(!obj)
	{	if((type&(DT_DELETE|DT_DETACH|DT_REMOVE)) && (dt->meth->type&(DT_STACK|DT_QUEUE)) )
			if((r = list->link) ) /* special case for destack or dequeue */
				goto dt_delete;
		DTRETURN(obj, NULL); /* error, needing non-void object */
	}

	if(type&DT_RELINK) /* relink object after some processing */
	{	r = (Dtlink_t*)obj;
		goto do_insert;
	}
	else if(type&(DT_INSERT|DT_INSTALL|DT_APPEND|DT_ATTACH))
	{	if(!(r = _dtmake(dt, obj, type)) )
			DTRETURN(obj, NULL);
		dt->data->size += 1;

	do_insert:
		if(dt->meth->type&DT_DEQUE)
		{	if(type&DT_APPEND)
				goto dt_queue; /* append at end */
			else	goto dt_stack; /* insert at top */
		}
		else if(dt->meth->type&DT_LIST)
		{	if(type&DT_APPEND)
			{	if(!h || !h->_rght)
					goto dt_queue;
				r->_rght = h->_rght;
				r->_rght->_left = r;
				r->_left = h;
				r->_left->_rght = r;
			}
			else
			{	if(!h || h == list->link )
					goto dt_stack;
				r->_left = h->_left;
				r->_left->_rght = r;
				r->_rght = h;
				r->_rght->_left = r;
			}
		}
		else if(dt->meth->type&DT_STACK)
		{ dt_stack:
			r->_rght = t = list->link;
			if(t)
			{	r->_left = t->_left;
				t->_left = r;
			}
			else	r->_left = r;
			list->link = r;
		}
		else /* if(dt->meth->type&DT_QUEUE) */
		{ dt_queue:
			if((t = list->link) )
			{	t->_left->_rght = r;
				r->_left = t->_left;
				t->_left = r;
			}
			else
			{	list->link = r;
				r->_left = r;
			}
			r->_rght = NULL;
		}

		list->here = r;
		DTRETURN(obj, _DTOBJ(disc,r));
	}

	/* define key to match */
	if(type&DT_MATCH)
	{	key = obj;
		obj = NULL;
	}
	else	key = _DTKEY(disc, obj);

	/* try to find a matching object */
	if(h && _DTOBJ(disc,h) == obj && (type & (DT_SEARCH|DT_NEXT|DT_PREV)) )
		r = h; /* match at the finger, no search needed */
	else /* linear search through the list */
	{	h = NULL; /* track first/last obj with same key */
		for(r = list->link; r; r = r->_rght)
		{	o = _DTOBJ(disc,r); k = _DTKEY(disc,o);
			if(_DTCMP(dt, key, k, disc) != 0)
				continue;
			else if(type & (DT_REMOVE|DT_NEXT|DT_PREV) )
			{	if(o == obj) /* got exact object, done */
					break;
				else if(type&DT_NEXT) /* track last object */
					h = r;
				else if(type&DT_PREV) /* track first object */
					h = h ? h : r;
				else	continue;
			}
			else if(type & DT_ATLEAST )
				h = r; /* track last object */
			else	break;
		}
		r = h ? h : r;
	}
	if(!r) /* not found */
		DTRETURN(obj, NULL);

	if(type&(DT_DELETE|DT_DETACH|DT_REMOVE))
	{ dt_delete:
		if(r->_rght)
			r->_rght->_left = r->_left;
		if(r == (t = list->link) )
		{	list->link = r->_rght;
			if((h = list->link) )
				h->_left = t->_left;
		}
		else
		{	r->_left->_rght = r->_rght;
			if(r == t->_left)
				t->_left = r->_left;
		}

		list->here = r == list->here ? r->_rght : NULL;

		obj = _DTOBJ(disc,r);
		_dtfree(dt, r, type);
		dt->data->size -= 1;

		DTRETURN(obj, obj);
	}

	if(type&DT_NEXT)
		r = r->_rght;
	else if(type&DT_PREV)
		r = r == list->link ? NULL : r->_left;
	/* else: if(type&(DT_SEARCH|DT_MATCH|DT_ATLEAST|DT_ATMOST)) */

	list->here = r;
	if(r)
		DTRETURN(obj, _DTOBJ(disc,r));
	else	DTRETURN(obj, NULL);

dt_return:
	DTANNOUNCE(dt,obj,type);
	DTCLRLOCK(dt);
	return obj;
}

static int listevent(Dt_t* dt, int event, void* arg)
{
	Dtlist_t	*list = (Dtlist_t*)dt->data;

	NOT_USED(arg);
	if(event == DT_OPEN)
	{	if(list) /* already initialized */
			return 0;
		if(!(list = (Dtlist_t*)(*dt->memoryf)(dt, 0, sizeof(Dtlist_t), dt->disc)) )
		{	DTERROR(dt, "Error in allocating a list data structure");
			return -1;
		}
		memset(list, 0, sizeof(Dtlist_t));
		dt->data = (Dtdata_t*)list;
		return 1;
	}
	else if(event == DT_CLOSE)
	{	if(!list) /* already closed */
			return 0;
		if(list->link) /* remove all items */
			(void)lclear(dt);
		(void)(*dt->memoryf)(dt, list, 0, dt->disc);
		dt->data = NULL;
		return 0;
	}
	else	return 0;
}

static Dtmethod_t _Dtlist  = { dtlist, DT_LIST,  listevent, "Dtlist"  };
static Dtmethod_t _Dtdeque = { dtlist, DT_DEQUE, listevent, "Dtdeque" };
static Dtmethod_t _Dtstack = { dtlist, DT_STACK, listevent, "Dtstack" };
static Dtmethod_t _Dtqueue = { dtlist, DT_QUEUE, listevent, "Dtqueue" };

Dtmethod_t*	Dtlist = &_Dtlist;
Dtmethod_t*	Dtdeque = &_Dtdeque;
Dtmethod_t*	Dtstack = &_Dtstack;
Dtmethod_t*	Dtqueue = &_Dtqueue;

#ifdef NoF
NoF(dtlist)
#endif
