/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1985-2011 AT&T Intellectual Property          *
*          Copyright (c) 2020-2024 Contributors to ksh 93u+m           *
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
*                                                                      *
***********************************************************************/

/*
 * dtopen() with handle placed in specific vm region
 */

#include <dt.h>

typedef struct Dc_s
{
	Dtdisc_t	ndisc;
	Dtdisc_t*	odisc;
} Dc_t;

static int
eventf(Dt_t* dt, int op, void* data, Dtdisc_t* disc)
{
	Dc_t*	dc = (Dc_t*)disc;
	int	r;

	if (dc->odisc->eventf && (r = (*dc->odisc->eventf)(dt, op, data, dc->odisc)))
		return r;
	return op == DT_ENDOPEN ? 1 : 0;
}

static void*
memoryf(Dt_t* dt, void* addr, size_t size, Dtdisc_t* disc)
{
	/* TODO: work out if new bytes need to be initialised to zero */
	return realloc(addr, size);
}

/*
 * open a dictionary using disc->memoryf if set or realloc otherwise
 */

Dt_t*
_dtnew(Dtdisc_t* disc, Dtmethod_t* meth, unsigned long version)
{
	Dt_t*		dt;
	Dc_t		dc;

	dc.odisc = disc;
	dc.ndisc = *disc;
	dc.ndisc.eventf = eventf;
	if (!dc.ndisc.memoryf)
		dc.ndisc.memoryf = memoryf;
	if (dt = _dtopen(&dc.ndisc, meth, version))
		dtdisc(dt, disc, DT_SAMECMP|DT_SAMEHASH);
	return dt;
}

#undef dtnew

Dt_t*
dtnew(Dtdisc_t* disc, Dtmethod_t* meth)
{
	return _dtnew(disc, meth, 20050420L);
}
