/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1982-2014 AT&T Intellectual Property          *
*          Copyright (c) 2020-2025 Contributors to ksh 93u+m           *
*                      and is licensed under the                       *
*                 Eclipse Public License, Version 2.0                  *
*                                                                      *
*                A copy of the License is available at                 *
*      https://www.eclipse.org/org/documents/epl-2.0/EPL-2.0.html      *
*         (with md5 checksum 84283fa8859daf213bdda5a9f8d1be1d)         *
*                                                                      *
*                  David Korn <dgk@research.att.com>                   *
*                  Martijn Dekker <martijn@inlv.org>                   *
*            Johnothan King <johnothanking@protonmail.com>             *
*         hyenias <58673227+hyenias@users.noreply.github.com>          *
*                Govind Kamat <govind_kamat@yahoo.com>                 *
*               Vincent Mihalkovic <vmihalko@redhat.com>               *
*                                                                      *
***********************************************************************/
/*
 *   History file manipulation routines
 *
 *   David Korn
 *   AT&T Labs
 *
 */

/*
 * Each command in the history file starts on an even byte and is null-terminated.
 * The first byte must contain the special character HIST_UNDO and the second
 * byte is the version number.  The sequence HIST_UNDO 0, following a command,
 * nullifies the previous command.  A six-byte sequence starting with
 * HIST_CMDNO is used to store the command number so that it is not necessary
 * to read the file from beginning to end to get to the last block of
 * commands.  This format of this sequence is different in version 1
 * than in version 0.  Version 1 allows commands to use the full 8-bit
 * character set.  It can understand version 0 format files.
 */


#include "shopt.h"
#include <ast.h>

#if !SHOPT_SCRIPTONLY

#define HIST_MAX	(sizeof(int)*HIST_BSIZE)
#define HIST_BIG	(0100000-1024)	/* 1K less than maximum short */
#define HIST_LINE	32		/* typical length for history line */
#define HIST_MARKSZ	6
#define HIST_RECENT	600
#define HIST_UNDO	0201		/* invalidate previous command */
#define HIST_CMDNO	0202		/* next 3 bytes give command number */
#define HIST_BSIZE	4096		/* size of history file buffer */
#define HIST_DFLT	512		/* default size of history list */

#if SHOPT_AUDIT
#   define _HIST_AUDIT	Sfio_t	*auditfp; \
			char	*tty; \
			int	auditmask;
#else
#   define _HIST_AUDIT
#endif

#define _HIST_PRIVATE \
	off_t	histcnt;	/* offset into history file */\
	off_t	histmarker;	/* offset of last command marker */ \
	int	histflush;	/* set if flushed outside of hflush() */\
	int	histmask;	/* power of two mask for histcnt */ \
	char	histbuff[HIST_BSIZE+1];	/* history file buffer */ \
	int	histwfail; \
	_HIST_AUDIT \
	off_t	histcmds[2];	/* offset for recent commands, must be last */

#define hist_ind(hp,c)	((int)((c)&(hp)->histmask))

#include	<sfio.h>
#include	"FEATURE/time"
#include	<error.h>
#include	<ls.h>
#include	"defs.h"
#include	"variables.h"
#include	"path.h"
#include	"builtins.h"
#include	"io.h"
#include	"history.h"

#ifndef O_BINARY
#   define O_BINARY	0
#endif /* O_BINARY */

int	_Hist = 0;
static void	hist_marker(char*,long);
static History_t* hist_trim(History_t*, int);
static int	hist_nearend(History_t*,Sfio_t*, off_t);
static int	hist_check(int);
static int	hist_clean(int);
static ssize_t	hist_write(Sfio_t*, const void*, size_t, Sfdisc_t*);
static int	hist_exceptf(Sfio_t*, int, void*, Sfdisc_t*);

static int	histinit;
static mode_t	histmode;
static History_t *hist_ptr;

#if SHOPT_ACCTFILE
    static int	acctfd;
    static char *logname;
#   include <pwd.h>

    static int  acctinit(History_t *hp)
    {
	char *cp, *acctfile;
	Namval_t *np = nv_search("ACCTFILE",sh.var_tree,0);

	if(!np || !(acctfile=nv_getval(np)))
		return 0;
	if(!(cp = getlogin()))
	{
		struct passwd *userinfo = getpwuid(getuid());
		if(userinfo)
			cp = userinfo->pw_name;
		else
			cp = "unknown";
	}
	logname = sh_strdup(cp);
	if((acctfd=sh_open(acctfile,
		O_BINARY|O_WRONLY|O_APPEND|O_CREAT,S_IRUSR|S_IWUSR))>=0 &&
	    (unsigned)acctfd < 10)
	{
		int n;
		if((n = sh_fcntl(acctfd, F_dupfd_cloexec, 10)) >= 0)
		{
			sh_close(acctfd);
			acctfd = n;
		}
	}
	if(acctfd < 0)
	{
		acctfd = 0;
		return 0;
	}
	if(sh_isdevfd(acctfile))
	{
		char newfile[16];
		sfsprintf(newfile,sizeof(newfile),"%.8s%d\0",e_devfdNN,acctfd);
		nv_putval(np,newfile,NV_RDONLY);
	}
	else
		fcntl(acctfd,F_SETFD,FD_CLOEXEC);
	return 1;
    }
#endif /* SHOPT_ACCTFILE */

#if SHOPT_AUDIT
static int sh_checkaudit(const char *name, char *logbuf, size_t len)
{
	char	*cp, *last;
	int	id1, id2, r=0, n, fd;
	if((fd=open(name, O_RDONLY,O_cloexec)) < 0)
		return 0;
	if((n = read(fd, logbuf,len-1)) < 0)
		goto done;
	while(logbuf[n-1]=='\n')
		n--;
	logbuf[n] = 0;
	if(!(cp=strchr(logbuf,';')) && !(cp=strchr(logbuf,' ')))
		goto done;
	*cp = 0;
	do
	{
		cp++;
		id1 = id2 = strtol(cp,&last,10);
		if(*last=='-')
			id1 = strtol(last+1,&last,10);
		if(sh.euserid >=id1 && sh.euserid <= id2)
			r |= 1;
		if(sh.userid >=id1 && sh.userid <= id2)
			r |= 2;
		cp = last;
	}
	while(*cp==';' ||  *cp==' ');
done:
	sh_close(fd);
	return r;
}
#endif /* SHOPT_AUDIT */

static const unsigned char hist_stamp[2] = { HIST_UNDO, HIST_VERSION };
static const Sfdisc_t hist_disc = { NULL, hist_write, NULL, hist_exceptf, NULL};

static void hist_touch(void *handle)
{
	touch((char*)handle, 0, 0, 0);
}

/*
 * open the history file
 * if HISTNAME is not given and userid==0 then no history file.
 * if HISTFILE is longer than HIST_MAX bytes then it is cleaned up.
 * sh_histinit() returns 1 if history file is open.
 */
int  sh_histinit(void)
{
	int fd;
	History_t *hp;
	char *histname;
	char *fname=0;
	int histmask, maxlines, hist_start=0;
	char *cp;
	off_t hsize = 0;

	if(sh.hist_ptr=hist_ptr)
		return 1;
	if(!(histname = nv_getval(HISTFILE)))
	{
		int offset = stktell(sh.stk);
		if(cp=nv_getval(HOME))
			sfputr(sh.stk,cp,-1);
		sfputr(sh.stk,hist_fname,0);
		stkseek(sh.stk,offset);
		histname = stkptr(sh.stk,offset);
	}
retry:
	cp = path_relative(histname);
	if(!histinit)
		histmode = S_IRUSR|S_IWUSR;
	if((fd=open(cp,O_BINARY|O_APPEND|O_RDWR|O_CREAT|O_cloexec,histmode))>=0)
	{
		hsize=lseek(fd,0,SEEK_END);
	}
	if((unsigned)fd < 10)
	{
		int n;
		if((n=sh_fcntl(fd,F_dupfd_cloexec,10))>=0)
		{
			sh_close(fd);
			fd=n;
		}
	}
	/* make sure that file has history file format */
	if(hsize && hist_check(fd))
	{
		sh_close(fd);
		hsize = 0;
		if(unlink(cp)>=0)
			goto retry;
		fd = -1;
	}
	if(fd < 0)
	{
		/* don't allow root a history_file in /tmp */
		if(sh.userid)
		{
			if(!(fname = pathtmp(NULL,0,0,NULL)))
				return 0;
			fd = open(fname,O_BINARY|O_APPEND|O_CREAT|O_RDWR,S_IRUSR|S_IWUSR|O_cloexec);
		}
	}
	if(fd<0)
		return 0;
	/* set the file to close-on-exec */
	fcntl(fd,F_SETFD,FD_CLOEXEC);
	if(cp=nv_getval(HISTSIZE))
	{
		intmax_t m = strtoll(cp, NULL, 10);
		if(m>HIST_MAX)
			m = HIST_MAX;
		else if(m<0)
			m = HIST_DFLT;
		maxlines = (int)m;
	}
	else
		maxlines = HIST_DFLT;
	for(histmask=16;histmask <= maxlines; histmask <<=1 );
	hp = new_of(History_t,(--histmask)*sizeof(off_t));
	sh.hist_ptr = hist_ptr = hp;
	hp->histsize = maxlines;
	hp->histmask = histmask;
	hp->histfp= sfnew(NULL,hp->histbuff,HIST_BSIZE,fd,SFIO_READ|SFIO_WRITE|SFIO_APPENDWR|SFIO_SHARE);
	memset((char*)hp->histcmds,0,sizeof(off_t)*(hp->histmask+1));
	hp->histind = 1;
	hp->histcmds[1] = 2;
	hp->histcnt = 2;
	hp->histname = sh_strdup(histname);
	hp->histdisc = hist_disc;
	if(hsize==0)
	{
		/* put special characters at front of file */
		sfwrite(hp->histfp,(char*)hist_stamp,2);
		sfsync(hp->histfp);
	}
	/* initialize history list */
	else
	{
		int first,last;
		off_t mark,size = (HIST_MAX/4)+maxlines*HIST_LINE;
		hp->histind = first = hist_nearend(hp,hp->histfp,hsize-size);
		histinit = 1;
		hist_eof(hp);	 /* this sets histind to last command */
		if((hist_start = (last=(int)hp->histind)-maxlines) <=0)
			hist_start = 1;
		mark = hp->histmarker;
		while(first > hist_start)
		{
			size += size;
			first = hist_nearend(hp,hp->histfp,hsize-size);
			hp->histind = first;
		}
		histinit = hist_start;
		hist_eof(hp);
		if(!histinit)
		{
			sfseek(hp->histfp,hp->histcnt=hsize,SEEK_SET);
			hp->histind = last;
			hp->histmarker = mark;
		}
		histinit = 0;
	}
	if(fname)
	{
		unlink(fname);
		free(fname);
	}
	if(hist_clean(fd) && hist_start>1 && hsize > HIST_MAX)
	{
#ifdef DEBUG
		sfprintf(sfstderr,"%jd: hist_trim hsize=%d\n",(Sflong_t)sh.current_pid,hsize);
		sfsync(sfstderr);
#endif /* DEBUG */
		hp = hist_trim(hp,(int)hp->histind-maxlines);
	}
	sfdisc(hp->histfp,&hp->histdisc);
	HISTCUR->nvalue = &hp->histind;
	sh_timeradd(1000L*(HIST_RECENT-30), 1, hist_touch, hp->histname);
#if SHOPT_ACCTFILE
	if(sh_isstate(SH_INTERACTIVE))
		acctinit(hp);
#endif /* SHOPT_ACCTFILE */
#if SHOPT_AUDIT
	{
		char buff[SFIO_BUFSIZE];
		hp->auditfp = 0;
		if(sh_isstate(SH_INTERACTIVE) && (hp->auditmask = sh_checkaudit(SHOPT_AUDITFILE, buff, sizeof(buff))))
		{
			if((fd=sh_open(buff,O_BINARY|O_WRONLY|O_APPEND|O_CREAT|O_cloexec,S_IRUSR|S_IWUSR))>=0 && fd < 10)
			{
				int n;
				if((n = sh_fcntl(fd,F_dupfd_cloexec, 10)) >= 0)
				{
					sh_close(fd);
					fd = n;
				}
			}
			if(fd>=0)
			{
				const char *tty;
				fcntl(fd,F_SETFD,FD_CLOEXEC);
				tty = ttyname(2);
				hp->tty = sh_strdup(tty?tty:"notty");
				hp->auditfp = sfnew(NULL,NULL,-1,fd,SFIO_WRITE);
			}
		}
	}
#endif
	return 1;
}

/*
 * close the history file and free the space
 */
void hist_close(History_t *hp)
{
	sfclose(hp->histfp);
#if SHOPT_AUDIT
	if(hp->auditfp)
	{
		if(hp->tty)
			free(hp->tty);
		sfclose(hp->auditfp);
	}
#endif /* SHOPT_AUDIT */
	free(hp);
	hist_ptr = 0;
	sh.hist_ptr = 0;
#if SHOPT_ACCTFILE
	if(acctfd)
	{
		sh_close(acctfd);
		acctfd = 0;
	}
#endif /* SHOPT_ACCTFILE */
}

/*
 * check history file format to see if it begins with special byte
 */
static int hist_check(int fd)
{
	unsigned char magic[2];
	lseek(fd,0,SEEK_SET);
	if((read(fd,(char*)magic,2)!=2) || (magic[0]!=HIST_UNDO))
		return 1;
	return 0;
}

/*
 * clean out history file OK if not modified in HIST_RECENT seconds
 */
static int hist_clean(int fd)
{
	struct stat statb;
	return fstat(fd,&statb)>=0 && (time(NULL)-statb.st_mtime) >= HIST_RECENT;
}

/*
 * Copy the last <n> commands to a new file and make this the history file
 */
static History_t* hist_trim(History_t *hp, int n)
{
	char *cp;
	int incmd=1, c=0;
	History_t *hist_new, *hist_old = hp;
	char *buff, *endbuff;
	off_t oldp,newp;
	struct stat statb;
	if(unlink(hist_old->histname) < 0)
	{
		errormsg(SH_DICT,ERROR_warn(0),"cannot trim history file %s; make sure parent directory is writable",hist_old->histname);
		return hist_ptr = hist_old;
	}
	hist_ptr = 0;
	if(fstat(sffileno(hist_old->histfp),&statb)>=0)
	{
		histinit = 1;
		histmode =  statb.st_mode;
	}
	if(!sh_histinit())
	{
		/* use the old history file */
		return hist_ptr = hist_old;
	}
	hist_new = hist_ptr;
	hist_ptr = hist_old;
	if(--n < 0)
		n = 0;
	newp = hist_seek(hist_old,++n);
	while(1)
	{
		if(!incmd)
		{
			c = hist_ind(hist_new,++hist_new->histind);
			hist_new->histcmds[c] = hist_new->histcnt;
			if(hist_new->histcnt > hist_new->histmarker+HIST_BSIZE/2)
			{
				char locbuff[HIST_MARKSZ];
				hist_marker(locbuff,hist_new->histind);
				sfwrite(hist_new->histfp,locbuff,HIST_MARKSZ);
				hist_new->histcnt += HIST_MARKSZ;
				hist_new->histmarker = hist_new->histcmds[hist_ind(hist_new,c)] = hist_new->histcnt;
			}
			oldp = newp;
			newp = hist_seek(hist_old,++n);
			if(newp <=oldp)
				break;
		}
		if(!(buff=(char*)sfreserve(hist_old->histfp,SFIO_UNBOUND,0)))
			break;
		*(endbuff=(cp=buff)+sfvalue(hist_old->histfp)) = 0;
		/* copy to null byte */
		incmd = 0;
		while(*cp++);
		if(cp > endbuff)
			incmd = 1;
		else if(*cp==0)
			cp++;
		if(cp > endbuff)
			cp = endbuff;
		c = cp-buff;
		hist_new->histcnt += c;
		sfwrite(hist_new->histfp,buff,c);
	}
	hist_cancel(hist_new);
	sfclose(hist_old->histfp);
	free(hist_old);
	return hist_ptr = hist_new;
}

/*
 * position history file at size and find next command number
 */
static int hist_nearend(History_t *hp, Sfio_t *iop, off_t size)
{
	unsigned char *cp, *endbuff;
	int n, incmd=1;
	unsigned char *buff, marker[4];
	if(size <= 2L || sfseek(iop,size,SEEK_SET)<0)
		goto begin;
	/* skip to marker command and return the number */
	/* numbering commands occur after a null and begin with HIST_CMDNO */
	while(cp=buff=(unsigned char*)sfreserve(iop,SFIO_UNBOUND,SFIO_LOCKR))
	{
		n = sfvalue(iop);
		*(endbuff=cp+n) = 0;
		while(1)
		{
			/* check for marker */
			if(!incmd && *cp++==HIST_CMDNO && *cp==0)
			{
				n = cp+1 - buff;
				incmd = -1;
				break;
			}
			incmd = 0;
			while(*cp++);
			if(cp>endbuff)
			{
				incmd = 1;
				break;
			}
			if(*cp==0 && ++cp>endbuff)
				break;
		}
		size += n;
		sfread(iop,(char*)buff,n);
		if(incmd < 0)
		{
			if((n=sfread(iop,(char*)marker,4))==4)
			{
				n = (marker[0]<<16)|(marker[1]<<8)|marker[2];
				if(n < size/2)
				{
					hp->histmarker = hp->histcnt = size+4;
					return n;
				}
				n=4;
			}
			if(n >0)
				size += n;
			incmd = 0;
		}
	}
begin:
	sfseek(iop,(off_t)2,SEEK_SET);
	hp->histmarker = hp->histcnt = 2L;
	return 1;
}

/*
 * This routine reads the history file from the present position
 * to the end-of-file and puts the information in the in-core
 * history table
 * Note that HIST_CMDNO is only recognized at the beginning of a command
 * and that HIST_UNDO as the first character of a command is skipped
 * unless it is followed by 0.  If followed by 0 then it cancels
 * the previous command.
 */
void hist_eof(History_t *hp)
{
	char *cp,*first,*endbuff;
	int incmd = 0;
	off_t count = hp->histcnt;
	int oldind=0,n,skip=0;
	off_t last = sfseek(hp->histfp,0,SEEK_END);
	if(last < count)
	{
		last = -1;
		count = 2+HIST_MARKSZ;
		oldind = hp->histind;
		if((hp->histind -= hp->histsize) < 0)
			hp->histind = 1;
	}
again:
	sfseek(hp->histfp,count,SEEK_SET);
	while(cp=(char*)sfreserve(hp->histfp,SFIO_UNBOUND,0))
	{
		n = sfvalue(hp->histfp);
		*(endbuff = cp+n) = 0;
		first = cp += skip;
		while(1)
		{
			while(!incmd)
			{
				if(cp>first)
				{
					count += (cp-first);
					n = hist_ind(hp, ++hp->histind);
						hp->histcmds[n] = count;
					first = cp;
				}
				switch(*((unsigned char*)(cp++)))
				{
					case HIST_CMDNO:
						if(*cp==0)
						{
							hp->histmarker=count+2;
							cp += (HIST_MARKSZ-1);
							hp->histind--;
							if(!histinit && (cp <= endbuff))
							{
								unsigned char *marker = (unsigned char*)(cp-4);
								hp->histind = ((marker[0]<<16)|(marker[1]<<8)|marker[2] -1);
							}
						}
						break;
					case HIST_UNDO:
						if(*cp==0)
						{
							cp+=1;
							hp->histind-=2;
						}
						break;
					default:
						cp--;
						incmd = 1;
				}
				if(cp > endbuff)
				{
					cp++;
					goto refill;
				}
			}
			first = cp;
			while(*cp++);
			if(cp > endbuff)
				break;
			incmd = 0;
			while(*cp==0)
			{
				if(++cp > endbuff)
					goto refill;
			}
		}
	refill:
		count += (--cp-first);
		skip = (cp-endbuff);
		if(!incmd && !skip)
			hp->histcmds[hist_ind(hp,++hp->histind)] = count;
	}
	hp->histcnt = count;
	if(incmd && last)
	{
		sfputc(hp->histfp,0);
		hist_cancel(hp);
		count = 2;
		skip = 0;
		oldind -= hp->histind;
		hp->histind = hp->histind-hp->histsize + oldind +2;
		if(hp->histind<0)
			hp->histind = 1;
		if(last<0)
		{
			char	buff[HIST_MARKSZ];
			int	fd = open(hp->histname,O_RDWR|O_cloexec);
			if(fd>=0)
			{
				hist_marker(buff,hp->histind);
				write(fd,(char*)hist_stamp,2);
				write(fd,buff,HIST_MARKSZ);
				sh_close(fd);
			}
		}
		last = 0;
		goto again;
	}
}

/*
 * This routine will cause the previous command to be cancelled
 */
void hist_cancel(History_t *hp)
{
	int c;
	if(!hp)
		return;
	sfputc(hp->histfp,HIST_UNDO);
	sfputc(hp->histfp,0);
	sfsync(hp->histfp);
	hp->histcnt += 2;
	c = hist_ind(hp,--hp->histind);
	hp->histcmds[c] = hp->histcnt;
}

/*
 * flush the current history command
 */
void hist_flush(History_t *hp)
{
	char *buff;
	if(hp)
	{
		if(buff=(char*)sfreserve(hp->histfp,0,SFIO_LOCKR))
		{
			hp->histflush = sfvalue(hp->histfp)+1;
			sfwrite(hp->histfp,buff,0);
		}
		else
			hp->histflush=0;
		if(sfsync(hp->histfp)<0)
		{
			hist_close(hp);
			if(!sh_histinit())
				sh_offoption(SH_HISTORY);
		}
		hp->histflush = 0;
	}
}

/*
 * This is the write discipline for the history file
 * When called from hist_flush(), trailing newlines are deleted and
 * a zero byte.  Line sequencing is added as required
 */
static ssize_t hist_write(Sfio_t *iop,const void *buff,size_t insize,Sfdisc_t* handle)
{
	History_t *hp = (History_t*)handle;
	char *bufptr = ((char*)buff)+insize;
	int c,size = insize;
	off_t cur;
	int saved=0;
	char saveptr[HIST_MARKSZ];
	if(!hp->histflush)
		return write(sffileno(iop),(char*)buff,size);
	if((cur = lseek(sffileno(iop),0,SEEK_END)) <0)
	{
		errormsg(SH_DICT,2,"hist_flush: EOF seek failed errno=%d",errno);
		return -1;
	}
	hp->histcnt = cur;
	/* remove whitespace from end of commands */
	while(--bufptr >= (char*)buff)
	{
		c= *bufptr;
		if(!isspace(c))
		{
			if(c=='\\' && *(bufptr+1)!='\n')
				bufptr++;
			break;
		}
	}
	/* don't count empty lines */
	if(++bufptr <= (char*)buff)
		return insize;
	*bufptr++ = '\n';
	*bufptr++ = 0;
	size = bufptr - (char*)buff;
#if	 SHOPT_AUDIT
	if(hp->auditfp)
	{
		time_t	t=time(NULL);
		sfprintf(hp->auditfp, "%u;%ju;%s;%*s%c",
			 sh_isoption(SH_PRIVILEGED) ? sh.euserid : sh.userid,
			 (Sfulong_t)t, hp->tty, size, buff, 0);
		sfsync(hp->auditfp);
	}
#endif	/* SHOPT_AUDIT */
#if	SHOPT_ACCTFILE
	if(acctfd)
	{
		int timechars, offset;
		offset = stktell(sh.stk);
		sfputr(sh.stk,buff,-1);
		stkseek(sh.stk,stktell(sh.stk) - 1);
		timechars = sfprintf(sh.stk, "\t%s\t%x\n",logname,time(NULL));
		lseek(acctfd, 0, SEEK_END);
		write(acctfd, stkptr(sh.stk,offset), size - 2 + timechars);
		stkseek(sh.stk,offset);

	}
#endif /* SHOPT_ACCTFILE */
	if(size&01)
	{
		size++;
		*bufptr++ = 0;
	}
	hp->histcnt +=  size;
	c = hist_ind(hp,++hp->histind);
	hp->histcmds[c] = hp->histcnt;
	if(hp->histflush>HIST_MARKSZ && hp->histcnt > hp->histmarker+HIST_BSIZE/2)
	{
		memcpy(saveptr,bufptr,HIST_MARKSZ);
		saved=1;
		hp->histcnt += HIST_MARKSZ;
		hist_marker(bufptr,hp->histind);
		hp->histmarker = hp->histcmds[hist_ind(hp,c)] = hp->histcnt;
		size += HIST_MARKSZ;
	}
	errno = 0;
	size = write(sffileno(iop),(char*)buff,size);
	if(saved)
		memcpy(bufptr,saveptr,HIST_MARKSZ);
	if(size>=0)
	{
		hp->histwfail = 0;
		return insize;
	}
	return -1;
}

/*
 * Put history sequence number <n> into buffer <buff>
 * The buffer must be large enough to hold HIST_MARKSZ chars
 */
static void hist_marker(char *buff,long cmdno)
{
	*buff++ = HIST_CMDNO;
	*buff++ = 0;
	*buff++ = (cmdno>>16);
	*buff++ = (cmdno>>8);
	*buff++ = cmdno;
	*buff++ = 0;
}

/*
 * return byte offset in history file for command <n>
 */
off_t hist_tell(History_t *hp, int n)
{
	return hp->histcmds[hist_ind(hp,n)];
}

/*
 * seek to the position of command <n>
 */
off_t hist_seek(History_t *hp, int n)
{
	if(!(n >= hist_min(hp) && n < hist_max(hp)))
		return -1;
	return sfseek(hp->histfp,hp->histcmds[hist_ind(hp,n)],SEEK_SET);
}

/*
 * write the command starting at offset <offset> onto file <outfile>.
 * if character <last> appears before newline it is deleted
 * each new-line character is replaced with string <nl>.
 */
void hist_list(History_t *hp,Sfio_t *outfile, off_t offset,int last, char *nl)
{
	int oldc=0;
	int c;
	if(offset<0 || !hp)
	{
		sfputr(outfile,sh_translate(e_unknown),'\n');
		return;
	}
	sfseek(hp->histfp,offset,SEEK_SET);
	while((c = sfgetc(hp->histfp)) != EOF)
	{
		if(c && oldc=='\n')
			sfputr(outfile,nl,-1);
		else if(last && (c==0 || (c=='\n' && oldc==last)))
			return;
		else if(oldc)
			sfputc(outfile,oldc);
		oldc = c;
		if(c==0)
			return;
	}
	return;
}

/*
 * find index for last line with given string
 * If flag==0 then line must begin with string
 * direction < 1 for backwards search
*/
Histloc_t hist_find(History_t*hp,char *string,int index1,int flag,int direction)
{
	int index2;
	off_t offset;
	int *coffset=0;
	Histloc_t location;
	location.hist_command = -1;
	location.hist_char = 0;
	location.hist_line = 0;
	if(!hp)
		return location;
	/* leading ^ means beginning of line unless escaped */
	if(flag)
	{
		index2 = *string;
		if(index2=='\\')
			string++;
		else if(index2=='^')
		{
			flag=0;
			string++;
		}
	}
	if(flag)
		coffset = &location.hist_char;
	index2 = (int)hp->histind;
	if(direction<0)
	{
		index2 -= hp->histsize;
		if(index2<1)
			index2 = 1;
		if(index1 <= index2)
			return location;
	}
	else if(index1 >= index2)
		return location;
	while(index1!=index2)
	{
		direction>0?++index1:--index1;
		offset = hist_tell(hp,index1);
		if((location.hist_line=hist_match(hp,offset,string,coffset))>=0)
		{
			location.hist_command = index1;
			return location;
		}
		/* allow a search to be aborted */
		if(sh.trapnote & SH_SIGSET)
			break;
	}
	return location;
}

/*
 * search for <string> in history file starting at location <offset>
 * If coffset==0 then line must begin with string
 * returns the line number of the match if successful, otherwise -1
 */
int hist_match(History_t *hp,off_t offset,char *string,int *coffset)
{
	unsigned char *first, *cp;
	int m,n,c=1,line=0;
	mbinit();
	sfseek(hp->histfp,offset,SEEK_SET);
	if(!(cp = first = (unsigned char*)sfgetr(hp->histfp,0,0)))
		return -1;
	m = sfvalue(hp->histfp);
	n = (int)strlen(string);
	while(m > n)
	{
		if(strncmp((char*)cp,string,n)==0)
		{
			if(coffset)
				*coffset = (cp-first);
			return line;
		}
		if(!coffset)
			break;
		if(*cp=='\n')
			line++;
		if((c=mbsize(cp)) < 0)
			c = 1;
		cp += c;
		m -= c;
	}
	return -1;
}


#if SHOPT_ESH || SHOPT_VSH
/*
 * copy command <command> from history file to s1
 * at most <size> characters copied
 * if s1==0 the number of lines for the command is returned
 * line=linenumber  for emacs copy and only this line of command will be copied
 * line < 0 for full command copy
 * -1 returned if there is no history file
 */
int hist_copy(char *s1,int size,int command,int line)
{
	int c;
	History_t *hp = sh.hist_ptr;
	int count = 0;
	char *const s1orig = s1;
	char *const s1max = s1 ? s1 + size : NULL;
	if(!hp)
		return -1;
	hist_seek(hp,command);
	while ((c = sfgetc(hp->histfp)) && c!=EOF)
	{
		if(c=='\n')
		{
			if(count++ ==line)
				break;
			else if(line >= 0)
				continue;
		}
		if(s1 && (line<0 || line==count))
		{
			if(s1 >= s1max)
			{
				*--s1 = 0;
				break;
			}
			*s1++ = c;
		}
	}
	sfseek(hp->histfp,0,SEEK_END);
	if(s1==0)
		return count;
	if(count && s1 > s1orig && (c = *(s1 - 1)) == '\n')
		s1--;
	*s1 = '\0';
	return count;
}

/*
 * return word number <word> from command number <command>
 */
char *hist_word(char *string,int size,int word)
{
	int c;
	int is_space;
	int quoted;
	char *s1 = string;
	unsigned char *cp = (unsigned char*)s1;
	int flag = 0;
	History_t *hp = hist_ptr;
	if(!hp)
		return NULL;
	hist_copy(string,size,(int)hp->histind-1,-1);
	for(quoted=0;c = *cp;cp++)
	{
		is_space = isspace(c) && !quoted;
		if(is_space && flag)
		{
			*cp = 0;
			if(--word==0)
				break;
			flag = 0;
		}
		else if(is_space==0 && flag==0)
		{
			s1 = (char*)cp;
			flag++;
		}
		if (c=='\'' && !quoted)
		{
			for(cp++;*cp && *cp != c;cp++)
				;
		}
		else if (c=='\"' && !quoted)
		{
			for(cp++;*cp && (*cp != c || quoted);cp++)
				quoted = *cp=='\\' ? !quoted : 0;
		}
		quoted = *cp=='\\' ? !quoted : 0;
	}
	*cp = 0;
	if(s1 != string)
		/* We can't use strcpy() because the two buffers may overlap. */
		strcopy(string,s1);
	return string;
}

#endif	/* SHOPT_ESH */

#if SHOPT_ESH
/*
 * given the current command and line number,
 * and number of lines back or forward,
 * compute the new command and line number.
 */
Histloc_t hist_locate(History_t *hp,int command,int line,int lines)
{
	Histloc_t next;
	line += lines;
	if(!hp)
	{
		command = -1;
		goto done;
	}
	if(lines > 0)
	{
		int count;
		while(command <= hp->histind)
		{
			count = hist_copy(NULL,0, command,-1);
			if(count > line)
				goto done;
			line -= count;
			command++;
		}
	}
	else
	{
		int least = (int)hp->histind-hp->histsize;
		while(1)
		{
			if(line >=0)
				goto done;
			if(--command < least)
				break;
			line += hist_copy(NULL,0, command,-1);
		}
		command = -1;
	}
done:
	next.hist_line = line;
	next.hist_command = command;
	return next;
}
#endif	/* SHOPT_ESH */


/*
 * Handle history file exceptions
 */
static int hist_exceptf(Sfio_t* fp, int type, void *data, Sfdisc_t *handle)
{
	int newfd,oldfd;
	History_t *hp = (History_t*)handle;
	NOT_USED(data);
	if(type==SFIO_WRITE)
	{
		if(errno==ENOSPC || hp->histwfail++ >= 10)
			return 0;
		/* write failure could be NFS problem, try to reopen */
		sh_close(oldfd=sffileno(fp));
		if((newfd=open(hp->histname,O_BINARY|O_APPEND|O_CREAT|O_RDWR|O_cloexec,S_IRUSR|S_IWUSR)) >= 0)
		{
			if(sh_fcntl(newfd, F_dupfd_cloexec, oldfd) != oldfd)
				return -1;
			fcntl(oldfd,F_SETFD,FD_CLOEXEC);
			close(newfd);
			if(lseek(oldfd,0,SEEK_END) < hp->histcnt)
			{
				int index = hp->histind;
				lseek(oldfd,(off_t)2,SEEK_SET);
				hp->histcnt = 2;
				hp->histind = 1;
				hp->histcmds[1] = 2;
				hist_eof(hp);
				hp->histmarker = hp->histcnt;
				hp->histind = index;
			}
			return 1;
		}
		errormsg(SH_DICT,2,"History file write error-%d %s: file unrecoverable",errno,hp->histname);
		return -1;
	}
	return 0;
}

#else
NoN(history)
#endif /* !SHOPT_SCRIPTONLY */
