/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1985-2011 AT&T Intellectual Property          *
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
*                                                                      *
***********************************************************************/
#include	"sfdchdr.h"

/*	Discipline to turn \r\n into \n.
**	This is useful to deal with DOS text files.
**
**	Written by David Korn (03/18/1998).
*/

#define MINMAP	8
#define CHUNK	1024

struct map
{
	Sfoff_t	logical;
	Sfoff_t	physical;
};

typedef struct _dosdisc
{
	Sfdisc_t	disc;
	struct map	*maptable;
	int		mapsize;
	int		maptop;
	Sfoff_t		lhere;
	Sfoff_t		llast;
	Sfoff_t		lmax;
	Sfoff_t		pmax;
	Sfoff_t		phere;
	Sfoff_t		plast;
	Sfoff_t		begin;
	int		skip;
	void		*buff;
	char		last;
	char		extra;
	int		bsize;
} Dosdisc_t;

static void addmapping(Dosdisc_t *dp)
{
	int n;
	if((n=dp->maptop++)>=dp->mapsize)
	{
		dp->mapsize *= 2;
		if(!(dp->maptable=(struct map*)realloc(dp->maptable,(dp->mapsize+1)*sizeof(struct map))))
		{
			dp->maptop--;
			dp->mapsize *= 2;
			return;
		}
	}
	dp->maptable[n].physical = dp->phere;
	dp->maptable[n].logical = dp->lhere;
	dp->maptable[dp->maptop].logical=0;
}

static struct map *getmapping(Dosdisc_t *dp, Sfoff_t offset, int whence)
{
	struct map *mp;
	static struct map dummy;
	if(offset <= dp->begin)
	{
		dummy.logical = dummy.physical = offset;
		return &dummy;
	}
	if(!(mp=dp->maptable))
	{
		dummy.logical = dp->begin;
		dummy.physical = dummy.logical+1;
		return &dummy;
	}
	while((++mp)->logical && (whence==SEEK_CUR?mp->physical:mp->logical) <= offset);
	return mp-1;
}

static ssize_t dos_read(Sfio_t *iop, void *buff, size_t size, Sfdisc_t* disc)
{
	Dosdisc_t *dp = (Dosdisc_t*)disc;
	char *cp = (char*)buff, *first, *cpmax;
	int n, count, m;
	if(dp->extra)
	{
		dp->extra=0;
		*cp = dp->last;
		return 1;
	}
	while(1)
	{
		if((n = sfrd(iop,buff,size,disc)) <= 0)
			return n;
		dp->plast=dp->phere;
		dp->phere +=n;
		dp->llast = dp->lhere;
		cpmax = cp+n-1;
		if(dp->last=='\r' && *cp!='\n')
		{
			/* should insert a '\r' */ ;
		}
		dp->last = *cpmax;
		if(n>1)
			break;
		if(dp->last!='\r')
		{
			dp->lhere++;
			return 1;
		}
	}
	if(dp->last=='\r')
		n--;
	else if(dp->last!='\n' || cpmax[-1]!='\r')
		*cpmax = '\r';
	dp->lhere += n;
	while(1)
	{
		while(*cp++ != '\r');
		if(cp > cpmax || *cp=='\n')
			break;
	}
	dp->skip = cp-1 - (char*)buff;
	/* if not \r\n in buffer, just return */
	if((count = cpmax+1-cp) <=0)
	{
		*cpmax = dp->last;
		if(!dp->maptable)
			dp->begin +=n;
		dp->skip++;
		count=0;
		goto done;
	}
	if(!dp->maptable)
	{
		dp->begin += cp - (char*)buff-1;
		if(dp->maptable=(struct map*)malloc((MINMAP+1)*sizeof(struct map)))
		{
			dp->mapsize = MINMAP;
			dp->maptable[0].logical=  dp->begin;
			dp->maptable[0].physical = dp->maptable[0].logical+1;
			dp->maptable[1].logical=0;
			dp->maptop = 1;
		}
	}
	/* save original discipline inside buffer */
	if(count > dp->bsize && !(dp->buff = realloc(dp->buff, dp->bsize = count)))
		return -1;
	memcpy(dp->buff, cp, count);
	count=1;
	while(1)
	{
		first=cp;
		if(cp==cpmax)
			cp++;
		else
			while(*cp++ != '\r');
		if(cp<=cpmax && *cp!='\n')
			continue;
		if((m=(cp-first)-1) >0)
			memcpy(first-count, first, m);
		if(cp > cpmax)
			break;
		count++;
	}
	cpmax[-count] = dp->last;
	dp->lhere -= count;
done:
	if(dp->lhere>dp->lmax)
	{
		dp->lmax = dp->lhere;
		dp->pmax = dp->phere;
		if(dp->maptable && dp->lmax > dp->maptable[dp->maptop-1].logical+CHUNK)
			addmapping(dp);
	}
	return n-count;
}

/*
 * returns the current offset
 * <offset> must be in the current buffer
 * if <whence> is SEEK_CUR, physical offset converted to logical offset
 *  otherwise, logical offset is converted to physical offset
 */
static Sfoff_t cur_offset(Dosdisc_t *dp, Sfoff_t offset,Sfio_t *iop,int whence)
{
	Sfoff_t n,m=0;
	char *cp;

	if(whence==SEEK_CUR)
	{
		whence= -1;
		n = offset - dp->plast;
		iop->next = iop->data + n;
		offset =  dp->llast;
	}
	else
	{
		whence = 1;
		n = offset - dp->llast;
		offset = dp->plast;
	}
	offset +=n;
	if((n -= dp->skip) > 0)
	{
		m=whence;
		cp = (char*)dp->buff;
		while(n--)
		{
			if(*cp++=='\r' && *cp=='\n')
			{
				m += whence;
				if(whence>0)
					n++;
			}
		}
	}
	if(whence<0)
		iop->next += m;
	return offset+m;
}

static Sfoff_t dos_seek(Sfio_t *iop, Sfoff_t offset, int whence, Sfdisc_t* disc)
{
	Dosdisc_t *dp = (Dosdisc_t*)disc;
	struct map dummy, *mp=0;
	Sfoff_t physical;
	int n,size;
retry:
	switch(whence)
	{
	    case SEEK_CUR:
		offset = sfsk(iop, 0,SEEK_CUR,disc);
		if(offset<=dp->begin)
			return offset;
		/* check for seek outside buffer */
		if(offset==dp->phere)
			return dp->lhere;
		else if(offset==dp->plast)
			return dp->llast;
		else if(offset<dp->plast || offset>dp->phere)
			mp = getmapping(dp,offset,whence);
		break;
	    case SEEK_SET:
		/* check for seek outside buffer */
		if(offset<dp->llast || offset > dp->lhere)
			mp = getmapping(dp,offset,whence);
		break;
	    case SEEK_END:
		if(!dp->maptable)
			return sfsk(iop,offset,SEEK_END,disc);
		mp = &dummy;
		mp->physical = dp->plast;
		mp->logical = dp->llast;
		break;
	}
	if(sfsetbuf(iop,(char*)iop,0))
		size = sfvalue(iop);
	else
		size = iop->endb-iop->data;
	if(mp)
	{
		sfsk(iop,mp->physical,SEEK_SET,disc);
		dp->phere = mp->physical;
		dp->lhere = mp->logical;
		if((*disc->readf)(iop,iop->data,size,disc)<0)
			return -1;
	}
	while(1)
	{
		if(whence==SEEK_CUR && dp->phere>=offset)
			break;
		if(whence==SEEK_SET && dp->lhere>=offset)
			break;
		n=(*disc->readf)(iop,iop->data,size,disc);
		if(n < 0)
			return -1;
		if(n==0)
		{
			if(whence==SEEK_END && offset<0)
			{
				offset = dp->lhere;
				whence=SEEK_SET;
				goto retry;
			}
			break;
		}
	}
	if(whence==SEEK_END)
		offset += dp->lhere;
	else
	{
		physical = cur_offset(dp,offset,iop,whence);
		if(whence==SEEK_SET)
		{
			sfsk(iop, physical ,SEEK_SET,disc);
			dp->phere = physical;
			dp->lhere = offset;
		}
		else
			offset = physical;
	}
	return offset;
}

static int dos_except(Sfio_t *iop, int type, void *arg, Sfdisc_t *disc)
{
	Dosdisc_t *dp = (Dosdisc_t*)disc;
	NOT_USED(iop);
	NOT_USED(arg);
	if(type==SFIO_DPOP || type==SFIO_FINAL)
	{
		if(dp->bsize>0)
			free(dp->buff);
		if(dp->mapsize)
			free(dp->maptable);
		free(disc);
	}
	return 0;
}

int sfdcdos(Sfio_t *f)
{
	Dosdisc_t *dos;

	/* this is a readonly discipline */
	if(sfset(f,0,0)&SFIO_WRITE)
		return -1;

	if(!(dos = (Dosdisc_t*)malloc(sizeof(Dosdisc_t))) )
		return -1;
	memset(dos,'\0',sizeof(Dosdisc_t));

	dos->disc.readf = dos_read;
	dos->disc.writef = NULL;
	dos->disc.seekf = dos_seek;
	dos->disc.exceptf = dos_except;

	if(sfdisc(f,(Sfdisc_t*)dos) != (Sfdisc_t*)dos)
	{	free(dos);
		return -1;
	}

	return 0;
}
