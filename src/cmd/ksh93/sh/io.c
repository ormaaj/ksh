/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1982-2012 AT&T Intellectual Property          *
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
*          atheik <14833674+atheik@users.noreply.github.com>           *
*            Johnothan King <johnothanking@protonmail.com>             *
*               K. Eugene Carlson <kvngncrlsn@gmail.com>               *
*                                                                      *
***********************************************************************/

/*
 * Input/output file processing
 *
 *   David Korn
 *   AT&T Labs
 *
 */

#include	"shopt.h"
#include	"defs.h"
#include	<fcin.h>
#include	<ls.h>
#include	<stdarg.h>
#include	<regex.h>
#include	"variables.h"
#include	"path.h"
#include	"io.h"
#include	"jobs.h"
#include	"shlex.h"
#include	"shnodes.h"
#include	"history.h"
#include	"edit.h"
#include	"timeout.h"
#include	"FEATURE/externs"
#include	"FEATURE/dynamic"
#include	"FEATURE/poll"

#ifdef	FNDELAY
#   ifdef EAGAIN
#	if EAGAIN!=EWOULDBLOCK
#	    undef EAGAIN
#	    define EAGAIN       EWOULDBLOCK
#	endif
#   else
#	define EAGAIN   EWOULDBLOCK
#   endif /* EAGAIN */
#   ifndef O_NONBLOCK
#	define O_NONBLOCK	FNDELAY
#   endif /* !O_NONBLOCK */
#endif	/* FNDELAY */

#ifndef O_SERVICE
#   define O_SERVICE	O_NOCTTY
#endif

#ifndef ERROR_PIPE
#ifdef ECONNRESET
#define ERROR_PIPE(e)	((e)==EPIPE||(e)==ECONNRESET||(e)==EIO)
#else
#define ERROR_PIPE(e)	((e)==EPIPE||(e)==EIO)
#endif
#endif

#define RW_ALL	(S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR|S_IWGRP|S_IWOTH)

static void	*timeout;
static int	(*fdnotify)(int,int);

#if defined(_lib_socket) && defined(_sys_socket) && defined(_hdr_netinet_in)
#   include <sys/socket.h>
#   include <netdb.h>
#   include <netinet/in.h>
#   if !defined(htons) && !_lib_htons
#      define htons(x)	(x)
#   endif
#   if !defined(htonl) && !_lib_htonl
#      define htonl(x)	(x)
#   endif
#   if _pipe_socketpair && !_stream_peek
#      ifndef SHUT_RD
#         define SHUT_RD         0
#      endif
#      ifndef SHUT_WR
#         define SHUT_WR         1
#      endif
#      if _socketpair_shutdown_mode
#         define pipe(v) ((socketpair(AF_UNIX,SOCK_STREAM,0,v)<0||shutdown((v)[1],SHUT_RD)<0||fchmod((v)[1],S_IWUSR)<0||shutdown((v)[0],SHUT_WR)<0||fchmod((v)[0],S_IRUSR)<0)?(-1):0)
#      else
#         define pipe(v) ((socketpair(AF_UNIX,SOCK_STREAM,0,v)<0||shutdown((v)[1],SHUT_RD)<0||shutdown((v)[0],SHUT_WR)<0)?(-1):0)
#      endif
#   endif

#if !_lib_getaddrinfo

#undef	EAI_SYSTEM

#define EAI_SYSTEM		1

#undef	addrinfo
#undef	getaddrinfo
#undef	freeaddrinfo

#define addrinfo		local_addrinfo
#define getaddrinfo		local_getaddrinfo
#define freeaddrinfo		local_freeaddrinfo

struct addrinfo
{
	int			ai_flags;
	int			ai_family;
	int			ai_socktype;
	int			ai_protocol;
	socklen_t		ai_addrlen;
	struct sockaddr*	ai_addr;
	struct addrinfo*	ai_next;
};

static int
getaddrinfo(const char* node, const char* service, const struct addrinfo* hint, struct addrinfo **addr)
{
	unsigned long	    	ip_addr = 0;
	unsigned short	    	ip_port = 0;
	struct addrinfo*	ap;
	struct hostent*		hp;
	struct sockaddr_in*	ip;
	char*			prot;
	long			n;

	if (!(hp = gethostbyname(node)) || hp->h_addrtype!=AF_INET || hp->h_length>sizeof(struct in_addr))
	{
		errno = EADDRNOTAVAIL;
		return EAI_SYSTEM;
	}
	ip_addr = (unsigned long)((struct in_addr*)hp->h_addr)->s_addr;
	if ((n = strtol(service, &prot, 10)) > 0 && n <= USHRT_MAX && !*prot)
		ip_port = htons((unsigned short)n);
	else
	{
		struct servent*	sp;
		const char*	protocol = 0;

		if (hint)
			switch (hint->ai_socktype)
			{
			case SOCK_STREAM:
				switch (hint->ai_protocol)
				{
				case 0:
					protocol = "tcp";
					break;
#ifdef IPPROTO_SCTP
				case IPPROTO_SCTP:
					protocol = "sctp";
					break;
#endif
				}
				break;
			case SOCK_DGRAM:
				protocol = "udp";
				break;
			}
		if (!protocol)
		{
			errno =  EPROTONOSUPPORT;
			return 1;
		}
		if (sp = getservbyname(service, protocol))
			ip_port = sp->s_port;
	}
	if (!ip_port)
	{
		errno = EADDRNOTAVAIL;
		return EAI_SYSTEM;
	}
	ap = sh_newof(0, struct addrinfo, 1, sizeof(struct sockaddr_in));
	if (hint)
		*ap = *hint;
	ap->ai_family = hp->h_addrtype;
	ap->ai_addrlen 	= sizeof(struct sockaddr_in);
	ap->ai_addr = (struct sockaddr *)(ap+1);
	ip = (struct sockaddr_in *)ap->ai_addr;
	ip->sin_family = AF_INET;
	ip->sin_port = ip_port;
	ip->sin_addr.s_addr = ip_addr;
	*addr = ap;
	return 0;
}

static void
freeaddrinfo(struct addrinfo* ap)
{
	if (ap)
		free(ap);
}

#endif /* !_lib_getaddrinfo */

static int	onintr(struct addrinfo*);

/*
 * return <protocol>/<host>/<service> fd
 * If called with flags==O_NONBLOCK return 1 if protocol is supported
 */
static int
inetopen(const char* path, int flags)
{
	char*		s;
	char*		t;
	int			fd;
	int			oerrno;
	struct addrinfo		hint;
	struct addrinfo*	addr;
	struct addrinfo*	p;
	int			server = !!(flags&O_SERVICE);

	memset(&hint, 0, sizeof(hint));
	hint.ai_family = PF_UNSPEC;
	switch (path[0])
	{
#ifdef IPPROTO_SCTP
	case 's':
		if (path[1]!='c' || path[2]!='t' || path[3]!='p' || path[4]!='/')
		{
			errno = ENOTDIR;
			return -1;
		}
		hint.ai_socktype = SOCK_STREAM;
		hint.ai_protocol = IPPROTO_SCTP;
		path += 5;
		break;
#endif
	case 't':
		if (path[1]!='c' || path[2]!='p' || path[3]!='/')
		{
			errno = ENOTDIR;
			return -1;
		}
		hint.ai_socktype = SOCK_STREAM;
		path += 4;
		break;
	case 'u':
		if (path[1]!='d' || path[2]!='p' || path[3]!='/')
		{
			errno = ENOTDIR;
			return -1;
		}
		hint.ai_socktype = SOCK_DGRAM;
		path += 4;
		break;
	default:
		errno = ENOTDIR;
		return -1;
	}
	if(flags==O_NONBLOCK)
		return 1;
	s = sh_strdup(path);
	if (t = strchr(s, '/'))
	{
		*t++ = 0;
		if (streq(s, "local"))
			s = sh_strdup("localhost");
		fd = getaddrinfo(s, t, &hint, &addr);
	}
	else
		fd = -1;
	free(s);
	if (fd)
	{
		if (fd != EAI_SYSTEM)
			errno = ENOTDIR;
		return -1;
	}
	oerrno = errno;
	errno = 0;
	fd = -1;
	for (p = addr; p; p = p->ai_next)
	{
		/*
		 * some APIs don't take the hint
		 */

		if (!p->ai_protocol)
			p->ai_protocol = hint.ai_protocol;
		if (!p->ai_socktype)
			p->ai_socktype = hint.ai_socktype;
		while ((fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) >= 0)
		{
			if (server && !bind(fd, p->ai_addr, p->ai_addrlen) && !listen(fd, 5) || !server && !connect(fd, p->ai_addr, p->ai_addrlen))
				goto done;
			close(fd);
			fd = -1;
			if (errno != EINTR)
				break;
			if (onintr(addr))
				goto done;
		}
	}
 done:
	freeaddrinfo(addr);
	if (fd >= 0)
		errno = oerrno;
	return fd;
}

#else

#undef	O_SERVICE

#endif

struct fdsave
{
	int	orig_fd;	/* original file descriptor */
	int	save_fd;	/* saved file descriptor */
	int	subshell;	/* saved for subshell */
	char	*tname;		/* name used with >; */
};

static int  	subexcept(Sfio_t*, int, void*, Sfdisc_t*);
static int  	eval_exceptf(Sfio_t*, int, void*, Sfdisc_t*);
static int  	slowexcept(Sfio_t*, int, void*, Sfdisc_t*);
static int	pipeexcept(Sfio_t*, int, void*, Sfdisc_t*);
static ssize_t	piperead(Sfio_t*, void*, size_t, Sfdisc_t*);
static ssize_t	slowread(Sfio_t*, void*, size_t, Sfdisc_t*);
static ssize_t	subread(Sfio_t*, void*, size_t, Sfdisc_t*);
static ssize_t	tee_write(Sfio_t*,const void*,size_t,Sfdisc_t*);
static int	io_prompt(Sfio_t*,int);
static int	io_heredoc(struct ionod*, const char*, int);
static void	sftrack(Sfio_t*,int,void*);
static const Sfdisc_t eval_disc = { NULL, NULL, NULL, eval_exceptf, NULL};
static Sfdisc_t tee_disc = {NULL,tee_write,NULL,NULL,NULL};
static Sfio_t	*subopen(Sfio_t*, off_t, long);
static const Sfdisc_t sub_disc = { subread, 0, 0, subexcept, 0 };

struct subfile
{
	Sfdisc_t	disc;
	Sfio_t		*oldsp;
	off_t		offset;
	long		size;
	long		left;
};

struct Eof
{
	Namfun_t	hdr;
	int		fd;
};

static Sfdouble_t nget_cur_eof(Namval_t* np, Namfun_t *fp)
{
	struct Eof *ep = (struct Eof*)fp;
	Sfoff_t end, cur =lseek(ep->fd, 0, SEEK_CUR);
	if(*np->nvname=='C')
	        return (Sfdouble_t)cur;
	if(cur<0)
		return (Sfdouble_t)-1;
	end =lseek(ep->fd, 0, SEEK_END);
	lseek(ep->fd, 0, SEEK_CUR);
	return (Sfdouble_t)end;
}

static const Namdisc_t EOF_disc	= { sizeof(struct Eof), 0, 0, nget_cur_eof};

#define MATCH_BUFF	(64*1024)
struct Match
{
	Sfoff_t	offset;
	char	*base;
};

static struct fdsave	*filemap;
static short		filemapsize;

#define PSEUDOFD	(SHRT_MAX)

/* ======== input output and file copying ======== */

int  sh_iovalidfd(int fd)
{
	Sfio_t		**sftable = sh.sftable;
	int		n, **fdptrs = sh.fdptrs;
	unsigned char	*fdstatus = sh.fdstatus;
	long		max;
	if(fd<0)
		return 0;
	if(fd < sh.lim.open_max)
		return 1;
	max = astconf_long(CONF_OPEN_MAX);
	if(max > INT_MAX)
		max = INT_MAX;
	if(fd >= max)
	{
		errno = EBADF;
		return 0;
	}
	n = (fd+16)&~0xf;
	if(n > max)
		n = max;
	max = sh.lim.open_max;
	sh.sftable = (Sfio_t**)sh_calloc((n+1)*(sizeof(int*)+sizeof(Sfio_t*)+1),1);
	if(max)
		memcpy(sh.sftable,sftable,max*sizeof(Sfio_t*));
	sh.fdptrs = (int**)(&sh.sftable[n]);
	if(max)
		memcpy(sh.fdptrs,fdptrs,max*sizeof(int*));
	sh.fdstatus = (unsigned char*)(&sh.fdptrs[n]);
	if(max)
		memcpy(sh.fdstatus,fdstatus,max);
	if(sftable)
		free(sftable);
	sh.lim.open_max = n;
	return 1;
}

/*
 * Return the lowest numbered fd that is equal to or greater than
 * the requested 'min_fd' and which is not currently in use.
 */
int  sh_iosafefd(int min_fd)
{
	int	fd;
	while(1)
	{
		if(fcntl(min_fd, F_GETFD) == -1)
		{
			for(fd = 0; fd < sh.topfd; fd++)
			{
				if (filemap[fd].save_fd == min_fd || filemap[fd].orig_fd == min_fd)
					break;
			}
			if(fd == sh.topfd)
				break;
		}
		min_fd++;
	}
	return min_fd;
}

int  sh_inuse(int fd)
{
	return fd < sh.lim.open_max && sh.fdptrs[fd];
}

void sh_ioinit(void)
{
	filemapsize = 8;
	filemap = (struct fdsave*)sh_malloc(filemapsize*sizeof(struct fdsave));
	if(!sh_iovalidfd(16))
	{
		errormsg(SH_DICT,ERROR_PANIC,"open files limit insufficient");
		UNREACHABLE();
	}
	sh.sftable[0] = sfstdin;
	sh.sftable[1] = sfstdout;
	sh.sftable[2] = sfstderr;
	sfnotify(sftrack);
	sh_iostream(0);
	sh_iostream(1);
	/* all write streams are in the same pool and share outbuff */
	sh.outpool = sfopen(NULL,NULL,"sw");  /* pool identifier */
	sh.outbuff = (char*)sh_malloc(IOBSIZE+4);
	sh.errbuff = (char*)sh_malloc(IOBSIZE/4);
	sfsetbuf(sfstderr,sh.errbuff,IOBSIZE/4);
	sfsetbuf(sfstdout,sh.outbuff,IOBSIZE);
	sfpool(sfstdout,sh.outpool,SFIO_WRITE);
	sfpool(sfstderr,sh.outpool,SFIO_WRITE);
	sfset(sfstdout,SFIO_LINE,0);
	sfset(sfstderr,SFIO_LINE,0);
	sfset(sfstdin,SFIO_SHARE|SFIO_PUBLIC,1);
}

/*
 *  Handle output stream exceptions
 */
static int outexcept(Sfio_t *iop,int type,void *data,Sfdisc_t *handle)
{
	static int	active = 0;
	if(type==SFIO_DPOP || type==SFIO_FINAL)
		free(handle);
	else if(type==SFIO_WRITE && (*(ssize_t*)data)<0 && sffileno(iop)!=2)
		switch (errno)
		{
		case EINTR:
		case EPIPE:
#ifdef ECONNRESET
		case ECONNRESET:
#endif
#ifdef ESHUTDOWN
		case ESHUTDOWN:
#endif
			break;
		default:
			if(!active)
			{
				int mode = ((struct checkpt*)sh.jmplist)->mode;
				int save = errno;
				active = 1;
				((struct checkpt*)sh.jmplist)->mode = 0;
				sfpurge(iop);
				sfpool(iop,NULL,SFIO_WRITE);
				errno = save;
				/*
				 * Note: UNREACHABLE() is avoided here because this errormsg() call will return.
				 * The ERROR_system flag causes sh_exit() to be called, but if sh.jmplist->mode
				 * is 0 (see above), then sh_exit() neither exits nor longjmps. See fault.c.
				 */
				errormsg(SH_DICT,ERROR_system(1),e_badwrite,sffileno(iop));
				active = 0;
				((struct checkpt*)sh.jmplist)->mode = mode;
				sh_exit(1);
			}
			return -1;
		}
	return 0;
}

/*
 * create or initialize a stream corresponding to descriptor <fd>
 * a buffer with room for a sentinel is allocated for a read stream.
 * A discipline is inserted when read stream is a tty or a pipe
 * For output streams, the buffer is set to sh.output and put into
 * the sh.outpool synchronization pool
 */
Sfio_t *sh_iostream(int fd)
{
	Sfio_t *iop;
	int status = sh_iocheckfd(fd);
	int flags = SFIO_WRITE;
	char *bp;
	Sfdisc_t *dp;
	if(status==IOCLOSE)
	{
		switch(fd)
		{
		    case 0:
			return sfstdin;
		    case 1:
			return sfstdout;
		    case 2:
			return sfstderr;
		}
		return NULL;
	}
	if(status&IOREAD)
	{
		bp = (char *)sh_malloc(IOBSIZE+1);
		flags |= SFIO_READ;
		if(!(status&IOWRITE))
			flags &= ~SFIO_WRITE;
	}
	else
		bp = sh.outbuff;
	if(status&IODUP)
		flags |= SFIO_SHARE|SFIO_PUBLIC;
	if((iop = sh.sftable[fd]) && sffileno(iop)>=0)
	{
		if(status&IOTTY)
			sfset(iop,SFIO_LINE|SFIO_WCWIDTH,1);
		sfsetbuf(iop, bp, IOBSIZE);
	}
	else if(!(iop=sfnew((fd<=2?iop:0),bp,IOBSIZE,fd,flags)))
		return NULL;
	dp = sh_newof(0,Sfdisc_t,1,0);
	if(status&IOREAD)
	{
		sfset(iop,SFIO_MALLOC,1);
		if(!(status&IOWRITE))
			sfset(iop,SFIO_IOCHECK,1);
		dp->exceptf = slowexcept;
		if(status&IOTTY)
			dp->readf = slowread;
		else if(status&IONOSEEK)
		{
			dp->readf = piperead;
			sfset(iop, SFIO_IOINTR,1);
		}
		else
			dp->readf = 0;
		dp->seekf = 0;
		dp->writef = 0;
	}
	else
	{
		if((status&(IONOSEEK|IOTTY)) == IONOSEEK)
			dp->exceptf = pipeexcept;
		else
			dp->exceptf = outexcept;
		sfpool(iop,sh.outpool,SFIO_WRITE);
	}
	sfdisc(iop,dp);
	sh.sftable[fd] = iop;
	return iop;
}

/*
 * preserve the file descriptor or stream by moving it
 */
static void io_preserve(Sfio_t *sp, int f2)
{
	int fd;
	if(sp)
		fd = sfsetfd(sp,10);
	else
		fd = sh_fcntl(f2,F_DUPFD,10);
	if(f2==sh.infd)
		sh.infd = fd;
	if(fd<0)
	{
		sh.toomany = 1;
		((struct checkpt*)sh.jmplist)->mode = SH_JMPERREXIT;
		errormsg(SH_DICT,ERROR_system(1),e_toomany);
		UNREACHABLE();
	}
	if(f2 >= sh.lim.open_max)
		sh_iovalidfd(f2);
	if(sh.fdptrs[fd]=sh.fdptrs[f2])
	{
		if(f2==job.fd)
			job.fd=fd;
		*sh.fdptrs[fd] = fd;
		sh.fdptrs[f2] = 0;
	}
	sh.sftable[fd] = sp;
	sh.fdstatus[fd] = sh.fdstatus[f2];
	if(fcntl(f2,F_GETFD,0)&1)
	{
		fcntl(fd,F_SETFD,FD_CLOEXEC);
		sh.fdstatus[fd] |= IOCLEX;
	}
	sh.sftable[f2] = 0;
}

/*
 * Given a file descriptor <f1>, move it to a file descriptor number <f2>
 * If <f2> is needed move it, otherwise it is closed first.
 * The original stream <f1> is closed.
 *  The new file descriptor <f2> is returned;
 */
int sh_iorenumber(int f1,int f2)
{
	Sfio_t *sp = sh.sftable[f2];
	if(f1!=f2)
	{
		/* see whether file descriptor is in use */
		if(sh_inuse(f2) || (f2>2 && sp))
		{
			if(!(sh.inuse_bits&(1<<f2)))
				io_preserve(sp,f2);
			sp = 0;
		}
		else if(f2==0)
			sh.st.ioset = 1;
		sh_close(f2);
		if(f2<=2 && sp)
		{
			Sfio_t *spnew = sh_iostream(f1);
			sh.fdstatus[f2] = (sh.fdstatus[f1]&~IOCLEX);
			sfsetfd(spnew,f2);
			sfswap(spnew,sp);
			sfset(sp,SFIO_SHARE|SFIO_PUBLIC,1);
		}
		else
		{
			sh.fdstatus[f2] = (sh.fdstatus[f1]&~IOCLEX);
			if((f2 = sh_fcntl(f1,F_DUPFD, f2)) < 0)
			{
				errormsg(SH_DICT,ERROR_system(1),e_file+4);
				UNREACHABLE();
			}
			else if(f2 <= 2)
				sh_iostream(f2);
		}
		if(sp)
			sh.sftable[f1] = 0;
		if(sh.fdstatus[f1]!=IOCLOSE)
			sh_close(f1);
	}
	else if(sp)
	{
		sfsetfd(sp,f2);
		if(f2<=2)
			sfset(sp,SFIO_SHARE|SFIO_PUBLIC,1);
	}
	if(f2>=sh.lim.open_max)
		sh_iovalidfd(f2);
	return f2;
}

/*
 * close a file descriptor and update stream table and attributes
 */
int sh_close(int fd)
{
	Sfio_t *sp;
	int r = 0;
	if(fd<0)
	{
		errno = EBADF;
		return -1;
	}
	if(fd >= sh.lim.open_max)
		sh_iovalidfd(fd);
	if(!(sp=sh.sftable[fd]) || sfclose(sp) < 0)
	{
		int err=errno;
		if(fdnotify)
			(*fdnotify)(fd,SH_FDCLOSE);
		while((r=close(fd)) < 0 && errno==EINTR)
			errno = err;
	}
	if(fd>2)
		sh.sftable[fd] = 0;
	sh.fdstatus[fd] = IOCLOSE;
	if(sh.fdptrs[fd])
		*sh.fdptrs[fd] = -1;
	sh.fdptrs[fd] = 0;
	if(fd < 10)
		sh.inuse_bits &= ~(1<<fd);
	return r;
}

#ifdef O_SERVICE

static int
onintr(struct addrinfo* addr)
{
	if (sh.trapnote&SH_SIGSET)
	{
		freeaddrinfo(addr);
		sh_exit(SH_EXITSIG);
		return -1;
	}
	if (sh.trapnote)
		sh_chktrap();
	return 0;
}

#endif

/*
 * Mimic open(2) with checks for pseudo /dev/ files.
 */
int sh_open(const char *path, int flags, ...)
{
	Sfio_t		*sp;
	int		fd = -1;
	mode_t		mode;
	char		*e;
	va_list		ap;
	va_start(ap, flags);
	mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
	va_end(ap);
	errno = 0;
	if(path==0)
	{
		errno = EFAULT;
		return -1;
	}
	if(*path==0)
	{
		errno = ENOENT;
		return -1;
	}
	if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/')
	{
		switch (path[5])
		{
		case 'f':
			if (path[6]=='d' && path[7]=='/')
			{
				if(flags==O_NONBLOCK)
					return 1;
				fd = (int)strtol(path+8, &e, 10);
				if (*e)
					fd = -1;
			}
			break;
		case 's':
			if (path[6]=='t' && path[7]=='d')
				switch (path[8])
				{
				case 'e':
					if (path[9]=='r' && path[10]=='r' && !path[11])
						fd = 2;
					break;
				case 'i':
					if (path[9]=='n' && !path[10])
						fd = 0;
					break;
				case 'o':
					if (path[9]=='u' && path[10]=='t' && !path[11])
						fd = 1;
					break;
				}
		}
#ifdef O_SERVICE
		if (fd < 0)
		{
			if ((fd = inetopen(path+5, flags)) < 0 && errno != ENOTDIR)
				return -1;
			if(flags==O_NONBLOCK)
				return fd>=0;
			if (fd >= 0)
				goto ok;
		}
		if(flags==O_NONBLOCK)
			return 0;
#endif
	}
	if (fd >= 0)
	{
		int nfd= -1;
		if (flags & O_CREAT)
		{
			struct stat st;
			if (stat(path,&st) >=0)
				nfd = open(path,flags,st.st_mode);
		}
		else
			nfd = open(path,flags);
		if(nfd>=0)
		{
			fd = nfd;
			goto ok;
		}
		if((mode=sh_iocheckfd(fd))==IOCLOSE)
			return -1;
		flags &= O_ACCMODE;
		if(!(mode&IOWRITE) && ((flags==O_WRONLY) || (flags==O_RDWR)))
			return -1;
		if(!(mode&IOREAD) && ((flags==O_RDONLY) || (flags==O_RDWR)))
			return -1;
		if((fd=dup(fd))<0)
			return -1;
	}
	else
	{
		while((fd = open(path, flags, mode)) < 0)
			if(errno!=EINTR || sh.trapnote)
				return -1;
 	}
 ok:
	flags &= O_ACCMODE;
	if(flags==O_WRONLY)
		mode = IOWRITE;
	else if(flags==O_RDWR)
		mode = (IOREAD|IOWRITE);
	else
		mode = IOREAD;
	if(fd >= sh.lim.open_max)
		sh_iovalidfd(fd);
	if((sp = sh.sftable[fd]) && (sfset(sp,0,0) & SFIO_STRING))
	{
		int n,err=errno;
		if((n = sh_fcntl(fd,F_DUPFD,10)) >= 10)
		{
			while(close(fd) < 0 && errno == EINTR)
				errno = err;
			fd = n;
		}
	}
	sh.fdstatus[fd] = mode;
	return fd;
}

/*
 * Open a file for reading
 * On failure, print message.
 */
int sh_chkopen(const char *name)
{
	int fd = sh_open(name,O_RDONLY,0);
	if(fd < 0)
	{
		errormsg(SH_DICT,ERROR_system(1),e_open,name);
		UNREACHABLE();
	}
	return fd;
}

/*
 * move open file descriptor to a number > 2
 */
int sh_iomovefd(int fdold)
{
	int fdnew;
	if(fdold >= sh.lim.open_max)
		sh_iovalidfd(fdold);
	if(fdold<0 || fdold>2)
		return fdold;
	fdnew = sh_iomovefd(dup(fdold));
	sh.fdstatus[fdnew] = (sh.fdstatus[fdold]&~IOCLEX);
	close(fdold);
	sh.fdstatus[fdold] = IOCLOSE;
	return fdnew;
}

/*
 * create a pipe and print message on failure
 */
int	sh_pipe(int pv[])
{
	int fd[2];
#ifdef pipe
	if(sh_isoption(SH_POSIX))
		return sh_rpipe(pv);
#endif
	if(pipe(fd)<0 || (pv[0]=fd[0])<0 || (pv[1]=fd[1])<0)
	{
		errormsg(SH_DICT,ERROR_system(1),e_pipe);
		UNREACHABLE();
	}
	pv[0] = sh_iomovefd(pv[0]);
	pv[1] = sh_iomovefd(pv[1]);
	sh.fdstatus[pv[0]] = IONOSEEK|IOREAD;
	sh.fdstatus[pv[1]] = IONOSEEK|IOWRITE;
	sh_subsavefd(pv[0]);
	sh_subsavefd(pv[1]);
	return 0;
}

#ifndef pipe
   int	sh_rpipe(int pv[])
   {
   	return sh_pipe(pv);
   }
#else
#  undef pipe
   /* create a real pipe when pipe() is socketpair */
   int	sh_rpipe(int pv[])
   {
	int fd[2];
	if(pipe(fd)<0 || (pv[0]=fd[0])<0 || (pv[1]=fd[1])<0)
	{
		errormsg(SH_DICT,ERROR_system(1),e_pipe);
		UNREACHABLE();
	}
	pv[0] = sh_iomovefd(pv[0]);
	pv[1] = sh_iomovefd(pv[1]);
	sh.fdstatus[pv[0]] = IONOSEEK|IOREAD;
	sh.fdstatus[pv[1]] = IONOSEEK|IOWRITE;
	sh_subsavefd(pv[0]);
	sh_subsavefd(pv[1]);
	return 0;
   }
#endif

static int pat_seek(void *handle, const char *str, size_t sz)
{
	char **bp = (char**)handle;
	NOT_USED(sz);
	*bp = (char*)str;
	return -1;
}

static int pat_line(const regex_t* rp, const char *buff, size_t n)
{
	const char *cp=buff, *sp;
	while(n>0)
	{
		for(sp=cp; n-->0 && *cp++ != '\n';);
		if(regnexec(rp,sp,cp-sp, 0, NULL, 0)==0)
			return sp-buff;
	}
	return cp-buff;
}

static int io_patseek(regex_t *rp, Sfio_t* sp, int flags)
{
	char	*cp, *match;
	int	r, fd=sffileno(sp), close_exec = sh.fdstatus[fd]&IOCLEX;
	int	was_share,s=(PIPE_BUF>SFIO_BUFSIZE?SFIO_BUFSIZE:PIPE_BUF);
	size_t	n,m;
	sh.fdstatus[sffileno(sp)] |= IOCLEX;
	if(fd==0)
		was_share = sfset(sp,SFIO_SHARE,1);
	while((cp=sfreserve(sp, -s, SFIO_LOCKR)) || (cp=sfreserve(sp,SFIO_UNBOUND, SFIO_LOCKR)))
	{
		m = n = sfvalue(sp);
		while(n>0 && cp[n-1]!='\n')
			n--;
		if(n)
			m = n;
		r = regrexec(rp,cp,m,0,NULL, 0, '\n', &match, pat_seek);
		if(r<0)
			m = match-cp;
		else if(r==2)
		{
			if((m = pat_line(rp,cp,m)) < n)
				r = -1;
		}
		if(m && (flags&IOCOPY))
			sfwrite(sfstdout,cp,m);
		sfread(sp,cp,m);
		if(r<0)
			break;
	}
	if(!close_exec)
		sh.fdstatus[sffileno(sp)] &= ~IOCLEX;
	if(fd==0 && !(was_share&SFIO_SHARE))
		sfset(sp, SFIO_SHARE,0);
	return 0;
}

static Sfoff_t	file_offset(int fn, char *fname)
{
	Sfio_t		*sp = sh.sftable[fn];
	char		*cp;
	Sfoff_t		off;
	struct Eof	endf;
	Namval_t	*mp = nv_open("EOF",sh.var_tree,0);
	Namval_t	*pp = nv_open("CUR",sh.var_tree,0);
	memset(&endf,0,sizeof(struct Eof));
	endf.fd = fn;
	endf.hdr.disc = &EOF_disc;
	endf.hdr.nofree = 1;
	if(mp)
		nv_stack(mp, &endf.hdr);
	if(pp)
		nv_stack(pp, &endf.hdr);
	if(sp)
		sfsync(sp);
	off = sh_strnum(fname, &cp, 0);
	if(mp)
		nv_stack(mp, NULL);
	if(pp)
		nv_stack(pp, NULL);
	return *cp ? (Sfoff_t)-1 : off;
}

/*
 * close a pipe
 */
void sh_pclose(int pv[])
{
	if(pv[0]>=2)
		sh_close(pv[0]);
	if(pv[1]>=2)
		sh_close(pv[1]);
	pv[0] = pv[1] = -1;
}

static char *io_usename(char *name, int *perm, int fno, int mode)
{
	struct stat	statb;
	char		*tname, *sp, *ep, path[PATH_MAX+1];
	int		fd,r;
	if(mode==0)
	{
		if((fd = sh_open(name,O_RDONLY,0)) >= 0)
		{
			r = fstat(fd,&statb);
			close(fd);
			if(r)
				return 0;
			if(!S_ISREG(statb.st_mode))
				return 0;
		 	*perm = statb.st_mode&(RW_ALL|(S_IXUSR|S_IXGRP|S_IXOTH));
		}
		else if(fd < 0  && errno!=ENOENT)
			return 0;
	}
	while((fd=readlink(name, path, PATH_MAX)) >0)
	{
		name=path;
		name[fd] = 0;
	}
	stkseek(sh.stk,1);
	sfputr(sh.stk,name,0);
	pathcanon(stkptr(sh.stk,1),PATH_PHYSICAL);
	sp = ep = stkptr(sh.stk,1);
	if(ep = strrchr(sp,'/'))
	{
		memmove(stkptr(sh.stk,0),sp,++ep-sp);
		stkseek(sh.stk,ep-sp);
	}
	else
	{
		ep = sp;
		stkseek(sh.stk,0);
	}
	sfprintf(sh.stk, ".<#%jd_%d{;.tmp", (Sflong_t)sh.current_pid, fno);
	tname = stkfreeze(sh.stk,1);
	switch(mode)
	{
	    case 1:
		rename(tname,name);
		break;
	    default:
		unlink(tname);
		break;
	}
	return tname;
}

/*
 * I/O redirection
 * flag = 0: normal redirection; file descriptors are restored when command terminates
 * flag = 1: redirections persist
 * flag = 2: redirections persist, but file descriptors > 2 are closed when executing an external command
 * flag = 3: just open file and return; for use when called from $( < ...)
 * flag = SH_SHOWME for trace only
 */
int	sh_redirect(struct ionod *iop, int flag)
{
	Sfoff_t off;
	char *fname;
	int fd = -1, iof;
	const char *message = e_open;
	int o_mode;		/* mode flag for open */
	static char io_op[7];	/* used for -x trace info */
	int trunc=0, clexec=0, fn, traceon=0;
	int r, indx = sh.topfd, perm= -1;
	char *tname=0, *after="", *trace = sh.st.trap[SH_DEBUGTRAP];
	Namval_t *np=0;

	if(flag==2 && !sh_isoption(SH_POSIX))
		clexec = 1;
	if(iop)
		traceon = sh_trace(NULL,0);
	/*
	 * A command substitution will hang on exit, writing infinite '\0', if,
	 * within it, standard output (FD 1) is redirected for a built-in command
	 * that calls sh_subfork(), or redirected permanently using 'exec' or
	 * 'redirect'. This forking workaround is necessary to avoid that bug.
	 * For shared-state comsubs, forking is incorrect, so error out then.
	 * TODO: actually fix the bug and remove this workaround.
	 * (Note that sh.redir0 is set to 1 in xec.c immediately before processing
	 * redirections for any built-in command, including 'exec' and 'redirect'.)
	 */
	if(sh.subshell && sh.comsub && sh.redir0==1)
	{
		struct ionod *i;
		for(i = iop; i; i = i->ionxt)
		{
			if((i->iofile & IOUFD) != 1)
				continue;
			if(!sh.subshare)
			{
				sh_subfork();
				break;
			}
			if(flag==1 || flag==2)
			{
				errormsg(SH_DICT,ERROR_exit(1),"cannot redirect stdout inside shared-state comsub");
				UNREACHABLE();
			}
		}
	}
	for(; iop; iop = iop->ionxt)
	{
		iof = iop->iofile;
		fn = (iof & IOUFD);
		if(sh.redir0 && fn==0 && !(iof&IOMOV))
			sh.redir0 = 2;
		io_op[0] = '0'+(iof&IOUFD);
		if(iof&IOPUT)
		{
			io_op[1] = '>';
			o_mode = O_WRONLY|O_CREAT;
		}
		else
		{
			io_op[1] = '<';
			o_mode = O_RDONLY|O_NONBLOCK;
		}
		io_op[2] = 0;
		io_op[3] = 0;
		io_op[4] = 0;
		fname = iop->ioname;
		if(!(iof&IORAW))
		{
			if(iof&IOLSEEK)
			{
				struct argnod *ap = stkalloc(sh.stk,ARGVAL+strlen(iop->ioname));
				memset(ap, 0, ARGVAL);
				ap->argflag = ARG_MAC;
				strcpy(ap->argval,iop->ioname);
				fname=sh_macpat(ap,(iof&IOARITH)?ARG_ARITH:ARG_EXP);
			}
			else if(!(iof&IOPROCSUB))
				fname=sh_mactrim(fname,(!sh_isoption(SH_NOGLOB)&&sh_isoption(SH_INTERACTIVE))?2:0);
		}
		if((iof&IOPROCSUB) && !(iof&IOLSEEK))
		{
			/* handle process substitution passed to redirection */
			struct argnod *ap = stkalloc(sh.stk,ARGVAL+strlen(iop->ioname));
			memset(ap, 0, ARGVAL);
			if(iof&IOPUT)
				ap->argflag = ARG_RAW;
			else if(sh.subshell)
				sh_subtmpfile();
			ap->argchn.ap = (struct argnod*)fname;
			ap = sh_argprocsub(ap);
			fname = ap->argval;
		}
		errno=0;
		np = 0;
		if(iop->iovname)
		{
			np = nv_open(iop->iovname,sh.var_tree,NV_VARNAME);
			if(nv_isattr(np,NV_RDONLY))
			{
				errormsg(SH_DICT,ERROR_exit(1),e_readonly, nv_name(np));
				UNREACHABLE();
			}
			io_op[0] = '}';
			if((iof&IOLSEEK) || ((iof&IOMOV) && *fname=='-'))
				fn = nv_getnum(np);
		}
		if(fn>=sh.lim.open_max && !sh_iovalidfd(fn))
		{
			errormsg(SH_DICT,ERROR_system(1),e_file+4);
			UNREACHABLE();
		}
		if(iof&IOLSEEK)
		{
			io_op[2] = '#';
			if(iof&IOARITH)
			{
				strcpy(&io_op[3]," ((");
				after = "))";
			}
			else if(iof&IOCOPY)
				io_op[3] = '#';
			goto traceit;
		}
		if(*fname || (iof&(IODOC|IOSTRG))==(IODOC|IOSTRG))
		{
			if(iof&IODOC)
			{
				if(traceon)
					sfputr(sfstderr,io_op,'<');
				fd = io_heredoc(iop,fname,traceon);
				if(traceon && (flag==SH_SHOWME))
					sh_close(fd);
				fname = 0;
			}
			else if(iof&IOMOV)
			{
				int dupfd,toclose= -1;
				io_op[2] = '&';
				if((fd=fname[0])>='0' && fd<='9')
				{
					char *number = fname;
					dupfd = strtol(fname,&number,10);
					if(*number=='-')
					{
						toclose = dupfd;
						number++;
					}
					if(*number || !sh_iovalidfd(dupfd) || dupfd > IOUFD)
					{
						message = e_file;
						goto fail;
					}
					if(sh.subshell && dupfd==1)
					{
						if(sh.subshell)
							sh_subtmpfile();
						dupfd = sffileno(sfstdout);
					}
					else if(sh.sftable[dupfd])
						sfsync(sh.sftable[dupfd]);
				}
				else if(fd=='-' && fname[1]==0)
				{
					fd= -1;
					goto traceit;
				}
				else if(fd=='p' && fname[1]==0)
				{
					if(iof&IOPUT)
						dupfd = sh.coutpipe;
					else
						dupfd = sh.cpipe[0];
					if(flag)
						toclose = dupfd;
				}
				else
				{
					message = e_file;
					goto fail;
				}
				if(flag==SH_SHOWME)
					goto traceit;
				if((fd=sh_fcntl(dupfd,F_DUPFD,3))<0)
					goto fail;
				if(fd>= sh.lim.open_max)
					sh_iovalidfd(fd);
				sh_iocheckfd(dupfd);
				sh.fdstatus[fd] = (sh.fdstatus[dupfd]&~IOCLEX);
				if(toclose<0 && sh.fdstatus[fd]&IOREAD)
					sh.fdstatus[fd] |= IODUP;
				else if(dupfd==sh.cpipe[0])
					sh_pclose(sh.cpipe);
				else if(toclose>=0)
				{
					if(flag==0)
						sh_iosave(toclose,indx,NULL); /* save file descriptor */
					sh_close(toclose);
				}
			}
			else if(iof&IORDW)
			{
				if(sh_isoption(SH_RESTRICTED))
				{
					errormsg(SH_DICT,ERROR_exit(1),e_restricted,fname);
					UNREACHABLE();
				}
				io_op[2] = '>';
				o_mode = O_RDWR|O_CREAT;
				if(iof&IOREWRITE)
					trunc = io_op[2] = ';';
				goto openit;
			}
			else if(!(iof&IOPUT))
			{
				if(flag==SH_SHOWME)
					goto traceit;
				fd=sh_chkopen(fname);
			}
			else if(sh_isoption(SH_RESTRICTED))
			{
				errormsg(SH_DICT,ERROR_exit(1),e_restricted,fname);
				UNREACHABLE();
			}
			else
			{
				if(iof&IOAPP)
				{
					io_op[2] = '>';
					o_mode |= O_APPEND;
				}
				else if((iof&IOREWRITE) && (flag==0 || flag==1 || sh_subsavefd(fn)))
				{
					io_op[2] = ';';
					o_mode |= O_TRUNC;
					if(tname = io_usename(fname,&perm,fn,0))
						o_mode |= O_EXCL;
				}
				else
				{
					o_mode |= O_TRUNC;
					if(iof&IOCLOB)
						io_op[2] = '|';
					else if(sh_isoption(SH_NOCLOBBER))
					{
						struct stat sb;
						if(stat(fname,&sb)>=0)
						{
							if(S_ISREG(sb.st_mode))
							{
								errno = EEXIST;
								errormsg(SH_DICT,ERROR_system(1),e_exists,fname);
								UNREACHABLE();
							}
						}
						else
							o_mode |= O_EXCL;
					}
				}
			openit:
				if(flag!=SH_SHOWME)
				{
					if((fd=sh_open(tname?tname:fname,o_mode,RW_ALL)) <0)
					{
						errormsg(SH_DICT,ERROR_system(1),((o_mode&O_CREAT)?e_create:e_open),fname);
						UNREACHABLE();
					}
					if(perm>0)
#if _lib_fchmod
						fchmod(fd,perm);
#else
						chmod(tname,perm);
#endif
				}
			}
		traceit:
			if(traceon && fname)
			{
				if(np)
					sfprintf(sfstderr,"{%s",nv_name(np));
				sfprintf(sfstderr,"%s %s%s%c",io_op,fname,after,iop->ionxt?' ':'\n');
			}
			if(flag==SH_SHOWME)
				continue;
			if(trace && fname)
			{
				char *argv[7], **av=argv;
				av[3] = io_op;
				av[4] = fname;
				av[5] = 0;
				av[6] = 0;
				if(iof&IOARITH)
					av[5] = after;
				if(np)
				{
					av[0] = "{";
					av[1] = nv_name(np);
					av[2] = "}";
				}
				else
					av +=3;
				sh_debug(trace,NULL,NULL,av,ARG_NOGLOB);
			}
			if(iof&IOLSEEK)
			{
				Sfio_t *sp = sh.sftable[fn];
				r = sh.fdstatus[fn];
				if(!(r&(IOSEEK|IONOSEEK)))
					r = sh_iocheckfd(fn);
				sfsprintf(io_op,sizeof(io_op),"%d\0",fn);
				if(r==IOCLOSE)
				{
					fname = io_op;
					message = e_file;
					goto fail;
				}
				if(iof&IOARITH)
				{
					if(r&IONOSEEK)
					{
						fname = io_op;
						message = e_notseek;
						goto fail;
					}
					message = e_badseek;
					if((off = file_offset(fn,fname))<0)
						goto fail;
					if(sp)
					{
						off=sfseek(sp, off, SEEK_SET);
						sfsync(sp);
					}
					else
						off=lseek(fn, off, SEEK_SET);
					if(off<0)
						r = -1;
				}
				else
				{
					regex_t *rp;
					if(!(r&IOREAD))
					{
						message = e_noread;
						goto fail;
					}
					if(!(rp = regcache(fname, REG_SHELL|REG_NOSUB|REG_NEWLINE|REG_AUGMENTED|REG_FIRST|REG_LEFT|REG_RIGHT, &r)))
					{
						message = e_badpattern;
						goto fail;
					}
					if(!sp)
						sp = sh_iostream(fn);
					r=io_patseek(rp,sp,iof);
					if(sp && flag==3)
					{
						/* close stream but not fn */
						sfsetfd(sp,-1);
						sfclose(sp);
					}
				}
				if(r<0)
					goto fail;
				if(flag==3)
					return fn;
				continue;
			}
			if(!np)
			{
				if(flag==0 || tname || (flag==1 && fn==1 && (sh.fdstatus[fn]&IONOSEEK) && sh.outpipepid == sh.current_pid))
				{
					if(fd==fn)
					{
						if((r=sh_fcntl(fd,F_DUPFD,10)) > 0)
						{
							fd = r;
							sh_close(fn);
						}
					}
					sh_iosave(fn,indx,tname?fname:(trunc?Empty:0));
				}
				else if(sh_subsavefd(fn))
				{
					if(fd==fn)
					{
						if((r=sh_fcntl(fd,F_DUPFD,10)) > 0)
						{
							fd = r;
							sh_close(fn);
						}
					}
					sh_iosave(fn,indx|IOSUBSHELL,tname?fname:0);
				}
			}
			if(fd<0)
			{
				if(sh_inuse(fn) || (fn && fn==sh.infd))
				{
					if(fn>9 || !(sh.inuse_bits&(1<<fn)))
						io_preserve(sh.sftable[fn],fn);
				}
				sh_close(fn);
			}
			if(flag==3)
				return sh_iomovefd(fd);  /* ensure FD > 2 to make $(<file) work with std{in,out,err} closed */
			if(fd>=0)
			{
				if(np)
				{
					int32_t v;
					fn = fd;
					if(fd<10)
					{
						if((fn=sh_fcntl(fd,F_DUPFD,10)) < 0)
							goto fail;
						if(fn>=sh.lim.open_max && !sh_iovalidfd(fn))
							goto fail;
						if(flag!=2 || sh.subshell)
							sh_iosave(fn,indx|0x10000,tname?fname:(trunc?Empty:0));
						sh.fdstatus[fn] = sh.fdstatus[fd];
						sh_close(fd);
						fd = fn;
					}
					_nv_unset(np,0);
					nv_onattr(np,NV_INT32);
					v = fn;
					nv_putval(np,(char*)&v, NV_INT32);
					sh_iocheckfd(fd);
				}
				else
				{
					fd = sh_iorenumber(sh_iomovefd(fd),fn);
					if(fn>2 && fn<10)
						sh.inuse_bits |= (1<<fn);
				}
			}
			if(fd >2 && clexec)
			{
				fcntl(fd,F_SETFD,FD_CLOEXEC);
				sh.fdstatus[fd] |= IOCLEX;
			}
		}
		else
			goto fail;
	}
	return indx;
fail:
	errormsg(SH_DICT,ERROR_system(1),message,fname);
	UNREACHABLE();
}
/*
 * Create a tmp file for the here-document
 */
static int io_heredoc(struct ionod *iop, const char *name, int traceon)
{
	Sfio_t		*infile = 0, *outfile, *tmp;
	int		fd;
	Sfoff_t		off;
	if(!(iop->iofile&IOSTRG) && (!sh.heredocs || iop->iosize==0))
		return sh_open(e_devnull,O_RDONLY);
	/* create an unnamed temporary file */
	if(!(outfile=sftmp(0)))
	{
		errormsg(SH_DICT,ERROR_system(1),e_tmpcreate);
		UNREACHABLE();
	}
	if(iop->iofile&IOSTRG)
	{
		if(traceon)
			sfprintf(sfstderr,"< %s\n",name);
		sfputr(outfile,name,'\n');
	}
	else
	{
		/*
		 * the locking is only needed in case & blocks process
		 * here-docs so this can be eliminated in some cases
		 */
		struct flock	lock;
		int	fno = sffileno(sh.heredocs);
		if(fno>=0)
		{
			memset(&lock,0,sizeof(lock));
			lock.l_type = F_WRLCK;
			lock.l_whence = SEEK_SET;
			fcntl(fno,F_SETLKW,&lock);
			lock.l_type = F_UNLCK;
		}
		off = sftell(sh.heredocs);
		infile = subopen(sh.heredocs,iop->iooffset,iop->iosize);
		if(traceon)
		{
			char *cp = sh_fmtq(iop->iodelim);
			fd = (*cp=='$' || *cp=='\'')?' ':'\\';
			sfprintf(sfstderr," %c%s\n",fd,cp);
			sfdisc(outfile,&tee_disc);
		}
		tmp = outfile;
		if(fno>=0 && !(iop->iofile&IOQUOTE))
			tmp = sftmp(iop->iosize<IOBSIZE?iop->iosize:0);
		if(fno>=0 || (iop->iofile&IOQUOTE))
		{
			/* This is a quoted here-document, not expansion */
			sfmove(infile,tmp,SFIO_UNBOUND,-1);
			sfclose(infile);
			if(sffileno(tmp)>0)
			{
				sfsetbuf(tmp,sh_malloc(IOBSIZE+1),IOBSIZE);
				sfset(tmp,SFIO_MALLOC,1);
			}
			sfseek(sh.heredocs,off,SEEK_SET);
			if(fno>=0)
				fcntl(fno,F_SETLK,&lock);
			sfseek(tmp,0,SEEK_SET);
			infile = tmp;
		}
		if(!(iop->iofile&IOQUOTE))
		{
			sh_machere(infile,outfile,iop->ioname);
			if(infile)
				sfclose(infile);
		}
	}
	/* close stream outfile, but save file descriptor */
	fd = sffileno(outfile);
	sfsetfd(outfile,-1);
	sfclose(outfile);
	if(traceon && !(iop->iofile&IOSTRG))
		sfputr(sfstderr,iop->ioname,'\n');
	lseek(fd,0,SEEK_SET);
	sh.fdstatus[fd] = IOREAD;
	return fd;
}

/*
 * This write discipline also writes the output on standard error
 * This is used when tracing here-documents
 */
static ssize_t tee_write(Sfio_t *iop,const void *buff,size_t n,Sfdisc_t *unused)
{
	NOT_USED(unused);
	sfwrite(sfstderr,buff,n);
	return write(sffileno(iop),buff,n);
}

/*
 * copy file <origfd> into a save place
 * The saved file is set close-on-exec
 * if <origfd> < 0, then -origfd is saved, but not duped so that it
 *   will be closed with sh_iorestore.
 */
void sh_iosave(int origfd, int oldtop, char *name)
{
	int savefd;
	int flag = (oldtop&(IOSUBSHELL|IOPICKFD));
	oldtop &= ~(IOSUBSHELL|IOPICKFD);
	/* see if already saved, only save once */
	for(savefd=sh.topfd; --savefd>=oldtop; )
	{
		if(filemap[savefd].orig_fd == origfd)
			return;
	}
	/* make sure table is large enough */
	if(sh.topfd >= filemapsize)
	{
		char 	*cp, *oldptr = (char*)filemap;
		char 	*oldend = (char*)&filemap[filemapsize];
		long	moved;
		filemapsize += 8;
		filemap = (struct fdsave*)sh_realloc(filemap,filemapsize*sizeof(struct fdsave));
		if(moved = (char*)filemap - oldptr)
		{
			for(savefd=sh.lim.open_max; --savefd>=0; )
			{
				cp = (char*)sh.fdptrs[savefd];
				if(cp >= oldptr && cp < oldend)
					sh.fdptrs[savefd] = (int*)(cp+moved);
			}
		}
	}
#if SHOPT_DEVFD
	if(origfd <0)
	{
		savefd = origfd;
		origfd = -origfd;
	}
	else
#endif /* SHOPT_DEVFD */
	if(flag&IOPICKFD)
		savefd = -1;
	else
	{
		if((savefd = sh_fcntl(origfd, F_DUPFD, 10)) < 0 && errno!=EBADF)
		{
			sh.toomany=1;
			((struct checkpt*)sh.jmplist)->mode = SH_JMPERREXIT;
			errormsg(SH_DICT,ERROR_system(1),e_toomany);
			UNREACHABLE();
		}
	}
	filemap[sh.topfd].tname = name;
	filemap[sh.topfd].subshell = (flag&IOSUBSHELL);
	filemap[sh.topfd].orig_fd = origfd;
	filemap[sh.topfd++].save_fd = savefd;
	if(savefd >=0)
	{
		Sfio_t* sp = sh.sftable[origfd];
		/* make saved file close-on-exec */
		sh_fcntl(savefd,F_SETFD,FD_CLOEXEC);
		if(origfd==job.fd)
			job.fd = savefd;
		sh.fdstatus[savefd] = sh.fdstatus[origfd];
		sh.fdptrs[savefd] = &filemap[sh.topfd-1].save_fd;
		if(!(sh.sftable[savefd]=sp))
			return;
		sfsync(sp);
		if(origfd <=2)
		{
			/* copy standard stream to new stream */
			sp = sfswap(sp,NULL);
			sh.sftable[savefd] = sp;
		}
		else
			sh.sftable[origfd] = 0;
	}
}

/*
 *  close all saved file descriptors
 */
void	sh_iounsave(void)
{
	int fd, savefd, newfd;
	for(newfd=fd=0; fd < sh.topfd; fd++)
	{
		if((savefd = filemap[fd].save_fd)< 0)
			filemap[newfd++] = filemap[fd];
		else
		{
			sh.sftable[savefd] = 0;
			sh_close(savefd);
		}
	}
	sh.topfd = newfd;
}

/*
 *  restore saved file descriptors from <last> on
 */
void	sh_iorestore(int last, int jmpval)
{
	int origfd, savefd, fd;
	int flag = (last&IOSUBSHELL);
	last &= ~IOSUBSHELL;
	for (fd = sh.topfd - 1; fd >= last; fd--)
	{
		if(!flag && filemap[fd].subshell)
			continue;
		if(jmpval==SH_JMPSCRIPT)
		{
			if ((savefd = filemap[fd].save_fd) >= 0)
			{
				sh.sftable[savefd] = 0;
				sh_close(savefd);
			}
			continue;
		}
		origfd = filemap[fd].orig_fd;
		if(origfd<0)
		{
			/* this should never happen */
			savefd = filemap[fd].save_fd;
			sh.sftable[savefd] = 0;
			sh_close(savefd);
			return;
		}
		if(filemap[fd].tname == Empty && sh.exitval==0)
		{
			sfsync(NULL);
			ftruncate(origfd,lseek(origfd,0,SEEK_CUR));
		}
		else if(filemap[fd].tname)
			io_usename(filemap[fd].tname,NULL,origfd,sh.exitval?2:1);
		sh_close(origfd);
		if ((savefd = filemap[fd].save_fd) >= 0)
		{
			sh_fcntl(savefd, F_DUPFD, origfd);
			if(savefd==job.fd)
				job.fd=origfd;
			sh.fdstatus[origfd] = sh.fdstatus[savefd];
			/* turn off close-on-exec if flag if necessary */
			if(sh.fdstatus[origfd]&IOCLEX)
				fcntl(origfd,F_SETFD,FD_CLOEXEC);
			if(origfd<=2)
			{
				sfswap(sh.sftable[savefd],sh.sftable[origfd]);
				if(origfd==0)
					sh.st.ioset = 0;
			}
			else
				sh.sftable[origfd] = sh.sftable[savefd];
			sh.sftable[savefd] = 0;
			sh_close(savefd);
		}
		else
			sh.fdstatus[origfd] = IOCLOSE;
	}
	if(!flag)
	{
		/* keep file descriptors for subshell restore */
		for (fd = last ; fd < sh.topfd; fd++)
		{
			if(filemap[fd].subshell)
				filemap[last++] = filemap[fd];
		}
	}
	if(last < sh.topfd)
		sh.topfd = last;
}

/*
 * returns access information on open file <fd>
 * returns -1 for failure, 0 for success
 * <mode> is the same as for access()
 */
int sh_ioaccess(int fd,int mode)
{
	int flags;
	if(mode==X_OK)
		return -1;
	if((flags=sh_iocheckfd(fd))!=IOCLOSE)
	{
		if(mode==F_OK)
			return 0;
		if(mode==R_OK && (flags&IOREAD))
			return 0;
		if(mode==W_OK && (flags&IOWRITE))
			return 0;
	}
	return -1;
}

/*
 *  Handle interrupts for slow streams
 */
static int slowexcept(Sfio_t *iop,int type,void *data,Sfdisc_t *handle)
{
	int	n,fno;
	NOT_USED(data);
	if(type==SFIO_DPOP || type==SFIO_FINAL)
		free(handle);
	if(type==SFIO_WRITE && ERROR_PIPE(errno))
	{
		sfpurge(iop);
		return -1;
	}
	if(type!=SFIO_READ)
		return 0;
	if((sh.trapnote&(SH_SIGSET|SH_SIGTRAP)) && errno!=EIO && errno!=ENXIO)
		errno = EINTR;
	fno = sffileno(iop);
	if((n=sfvalue(iop))<=0)
	{
#ifndef FNDELAY
#   ifdef O_NDELAY
		if(errno==0 && (n=fcntl(fno,F_GETFL,0))&O_NDELAY)
		{
			n &= ~O_NDELAY;
			fcntl(fno, F_SETFL, n);
			return 1;
		}
#   endif /* O_NDELAY */
#endif /* !FNDELAY */
#ifdef O_NONBLOCK
		if(errno==EAGAIN)
		{
			n = fcntl(fno,F_GETFL,0);
			n &= ~O_NONBLOCK;
			fcntl(fno, F_SETFL, n);
			return 1;
		}
#endif /* O_NONBLOCK */
		if(errno!=EINTR)
			return 0;
		else if(sh.bltinfun && (sh.trapnote&SH_SIGTRAP) && sh.lastsig)
			return -1;
		n=1;
		sh_onstate(SH_TTYWAIT);
	}
	else
		n = 0;
	if(sh.bltinfun && sh.bltindata.sigset)
		return -1;
	errno = 0;
	if(sh.trapnote&SH_SIGSET)
	{
		if(isatty(fno))
			sfputc(sfstderr,'\n');
		sh_exit(SH_EXITSIG);
	}
	if(sh.trapnote&SH_SIGTRAP)
		sh_chktrap();
	return n;
}

/*
 * called when slowread times out
 */
static void time_grace(void *handle)
{
	NOT_USED(handle);
	timeout = 0;
	if(sh_isstate(SH_GRACE))
	{
		sh_offstate(SH_GRACE);
		if(!sh_isstate(SH_INTERACTIVE))
			return;
		((struct checkpt*)sh.jmplist)->mode = SH_JMPEXIT;
		errormsg(SH_DICT,2,e_timeout);
		sh.trapnote |= SH_SIGSET;
		return;
	}
	errormsg(SH_DICT,0,e_timewarn);
	sh_onstate(SH_GRACE);
	sigrelease(SIGALRM);
	sh.trapnote |= SH_SIGTRAP;
}

static ssize_t piperead(Sfio_t *iop,void *buff,size_t size,Sfdisc_t *handle)
{
	int fd = sffileno(iop);
	if(job.waitsafe && job.savesig)
	{
		job_lock();
		job_unlock();
	}
	if(sh.trapnote)
	{
		errno = EINTR;
		return -1;
	}
	if(sh_isstate(SH_INTERACTIVE) && fd==0 && io_prompt(iop,sh.nextprompt)<0 && errno==EIO)
		return 0;
	sh_onstate(SH_TTYWAIT);
	if(!(sh.fdstatus[fd]&IOCLEX) && (sfset(iop,0,0)&SFIO_SHARE))
		size = ed_read(sh.ed_context, fd, (char*)buff, size,0);
	else
		size = sfrd(iop,buff,size,handle);
	sh_offstate(SH_TTYWAIT);
	return size;
}
/*
 * This is the read discipline that is applied to slow devices
 * This routine takes care of prompting for input
 */
static ssize_t slowread(Sfio_t *iop,void *buff,size_t size,Sfdisc_t *handle)
{
	int	(*readf)(void*, int, char*, int, int);
	int	reedit=0, rsize, n, fno;
#if SHOPT_HISTEXPAND
	char    *xp=0;
#endif /* SHOPT_HISTEXPAND */
	NOT_USED(handle);
#if SHOPT_ESH
	if(sh_isoption(SH_EMACS) || sh_isoption(SH_GMACS))
		readf = ed_emacsread;
	else
#endif	/* SHOPT_ESH */
#if SHOPT_VSH
	/* In multibyte locales, vi handles the no-editor mode as well. TODO: is this actually still needed? */
	if(sh_isoption(SH_VI) || mbwide())
		readf = ed_viread;
	else
#endif	/* SHOPT_VSH */
		readf = ed_read;
	if(sh.trapnote)
	{
		errno = EINTR;
		return -1;
	}
	fno = sffileno(iop);
#ifdef O_NONBLOCK
	if((n=fcntl(fno,F_GETFL,0))!=-1 && n&O_NONBLOCK)
	{
		n &= ~O_NONBLOCK;
		fcntl(fno, F_SETFL, n);
	}
#endif /* O_NONBLOCK */
	while(1)
	{
		if(io_prompt(iop,sh.nextprompt)<0 && errno==EIO)
			return 0;
		if(sh.timeout)
			timeout = sh_timeradd(sh_isstate(SH_GRACE)?1000L*TGRACE:1000L*sh.timeout,0,time_grace,&sh);
		rsize = (*readf)(sh.ed_context, fno, (char*)buff, size, reedit);
		if(timeout)
			sh_timerdel(timeout);
		timeout=0;
#if SHOPT_HISTEXPAND
		if(rsize > 0 && *(char*)buff != '\n' && sh.nextprompt==1 && sh_isoption(SH_HISTEXPAND))
		{
			int r;
			((char*)buff)[rsize] = '\0';
			if(xp)
			{
				free(xp);
				xp = 0;
			}
			r = hist_expand(buff, &xp);
			if(r == HIST_PRINT && xp)
			{
				/* !event:p -- print history expansion without executing */
				sfputr(sfstderr, xp, -1);
				continue;
			}
			if((r & (HIST_EVENT|HIST_PRINT)) && !(r & HIST_ERROR) && xp)
			{
				strlcpy(buff, xp, size);
				rsize = strlen(buff);
#if SHOPT_ESH || SHOPT_VSH
				if(!sh_isoption(SH_HISTVERIFY) || readf==ed_read)
#endif /* SHOPT_ESH || SHOPT_VSH */
				{
					sfputr(sfstderr, xp, -1);
					break;
				}
				reedit = rsize - 1;
				continue;
			}
#if SHOPT_ESH || SHOPT_VSH
			if((r & HIST_ERROR) && sh_isoption(SH_HISTREEDIT))
			{
				reedit  = rsize - 1;
				continue;
			}
#endif /* SHOPT_ESH || SHOPT_VSH */
			if(r & (HIST_ERROR|HIST_PRINT))
			{
				*(char*)buff = '\n';
				rsize = 1;
			}
		}
#endif /* SHOPT_HISTEXPAND */
		break;
	}
	return rsize;
}

/*
 * check and return the attributes for a file descriptor
 */
int sh_iocheckfd(int fd)
{
	int flags, n;
	if((n=sh.fdstatus[fd])&IOCLOSE)
		return n;
	if(!(n&(IOREAD|IOWRITE)))
	{
#ifdef F_GETFL
		if((flags=fcntl(fd,F_GETFL,0)) < 0)
			return sh.fdstatus[fd]=IOCLOSE;
		if((flags&O_ACCMODE)!=O_WRONLY)
			n |= IOREAD;
		if((flags&O_ACCMODE)!=O_RDONLY)
			n |= IOWRITE;
#else
		struct stat statb;
		if((flags = fstat(fd,&statb))< 0)
			return sh.fdstatus[fd]=IOCLOSE;
		n |= (IOREAD|IOWRITE);
		if(read(fd,"",0) < 0)
			n &= ~IOREAD;
#endif /* F_GETFL */
	}
	if(!(n&(IOSEEK|IONOSEEK)))
	{
		struct stat statb;
		/* /dev/null check is a workaround for select bug */
		static ino_t null_ino;
		static dev_t null_dev;
		if(null_ino==0 && stat(e_devnull,&statb) >=0)
		{
			null_ino = statb.st_ino;
			null_dev = statb.st_dev;
		}
		if(tty_check(fd))
			n |= IOTTY;
		if(lseek(fd,0,SEEK_CUR)<0)
		{
			n |= IONOSEEK;
#ifdef S_ISSOCK
			if((fstat(fd,&statb)>=0) && S_ISSOCK(statb.st_mode))
			{
				n |= IOREAD|IOWRITE;
#   if _socketpair_shutdown_mode
				if(!(statb.st_mode&S_IRUSR))
					n &= ~IOREAD;
				else if(!(statb.st_mode&S_IWUSR))
					n &= ~IOWRITE;
#   endif
			}
#endif /* S_ISSOCK */
		}
		else if((fstat(fd,&statb)>=0) && (
			S_ISFIFO(statb.st_mode) ||
#ifdef S_ISSOCK
			S_ISSOCK(statb.st_mode) ||
#endif /* S_ISSOCK */
			/* The following is for sockets on the sgi */
			(statb.st_ino==0 && (statb.st_mode & ~(S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR|S_IWGRP|S_IWOTH|S_IXUSR|S_IXGRP|S_IXOTH|S_ISUID|S_ISGID))==0) ||
			(S_ISCHR(statb.st_mode) && (statb.st_ino!=null_ino || statb.st_dev!=null_dev))
		))
			n |= IONOSEEK;
		else
			n |= IOSEEK;
	}
	if(fd==0)
		n &= ~IOWRITE;
	else if(fd==1)
		n &= ~IOREAD;
	sh.fdstatus[fd] = n;
	return n;
}

/*
 * Display prompt PS<flag> on standard error
 */
static int	io_prompt(Sfio_t *iop,int flag)
{
	char *cp;
	char buff[1];
	char *endprompt;
	static short cmdno;
	int sfflags;
	if(flag<3 && !sh_isstate(SH_INTERACTIVE))
		flag = 0;
	if(flag==2 && sfpkrd(sffileno(iop),buff,1,'\n',0,1) >= 0)
		flag = 0;
	if(flag==0)
		return sfsync(sfstderr);
	sfflags = sfset(sfstderr,SFIO_SHARE|SFIO_PUBLIC|SFIO_READ,0);
	if(!(sh.prompt=(char*)sfreserve(sfstderr,0,0)))
		sh.prompt = "";
	switch(flag)
	{
		case 1:
		{
			int c;
			sh_lexopen(sh.lex_context, 0);   /* reset lexer state */
			cp = sh_mactry(nv_getval(sh_scoped(PS1NOD)));
			sh.exitval = 0;  /* avoid sending a signal on termination */
			for(;c= *cp;cp++)
			{
				if(c==HIST_CHAR)
				{
					/* look at next character */
					c = *++cp;
					/* print out line number if not !! */
					if(c!= HIST_CHAR)
						sfprintf(sfstderr,"%d", sh.hist_ptr?(int)sh.hist_ptr->histind:++cmdno);
					if(c==0)
						break;
				}
				sfputc(sfstderr,c);
			}
			break;
		}
		case 2:
		{
			/* PS2 prompt. Save stack state to avoid corrupting command substitutions
			 * in case we're executing a PS2.get discipline function at parse time. */
			int	savestacktop = stktell(sh.stk);
			void	*savestackptr = stkfreeze(sh.stk,0);
			if (cp = nv_getval(sh_scoped(PS2NOD)))
				sfputr(sfstderr,cp,-1);
			/* Restore the stack. (If nv_getval ran a PS2.get discipline, this may free the space cp points to.) */
			stkset(sh.stk, savestackptr, savestacktop);
			break;
		}
		case 3:
			if (cp = nv_getval(sh_scoped(PS3NOD)))
				sfputr(sfstderr,cp,-1);
			break;
	}
	if(*sh.prompt && (endprompt=(char*)sfreserve(sfstderr,0,0)))
		*endprompt = 0;
	sfset(sfstderr,sfflags&SFIO_READ|SFIO_SHARE|SFIO_PUBLIC,1);
	return sfsync(sfstderr);
}

/*
 * This discipline is inserted on write pipes to prevent SIGPIPE
 * from causing an infinite loop
 */
static int pipeexcept(Sfio_t* iop, int mode, void *data, Sfdisc_t* handle)
{
	NOT_USED(data);
	if(mode==SFIO_DPOP || mode==SFIO_FINAL)
		free(handle);
	else if(mode==SFIO_WRITE && ERROR_PIPE(errno))
	{
		sfpurge(iop);
		return -1;
	}
	return 0;
}

/*
 * keep track of each stream that is opened and closed
 */
static void	sftrack(Sfio_t* sp, int flag, void* data)
{
	int fd = sffileno(sp);
	struct checkpt *pp;
	int mode;
	int newfd = integralof(data);
	if(flag==SFIO_SETFD || flag==SFIO_CLOSING)
	{
		if(newfd<0)
			flag = SFIO_CLOSING;
		if(fdnotify)
			(*fdnotify)(sffileno(sp),flag==SFIO_CLOSING?-1:newfd);
	}
#ifdef DEBUG
	if(flag==SFIO_READ || flag==SFIO_WRITE)
	{
		char *z = fmtint(sh.current_pid,0);
		write(ERRIO,z,strlen(z));
		write(ERRIO,": ",2);
		write(ERRIO,"attempt to ",11);
		if(flag==SFIO_READ)
			write(ERRIO,"read from",9);
		else
			write(ERRIO,"write to",8);
		write(ERRIO," locked stream\n",15);
		return;
	}
#endif
	if(fd<0 || fd==PSEUDOFD || (fd>=sh.lim.open_max && !sh_iovalidfd(fd)))
		return;
	if(sh_isstate(SH_NOTRACK))
		return;
	mode = sfset(sp,0,0);
	if(sp==sh.heredocs && fd < 10 && flag==SFIO_SETFD)
	{
		fd = sfsetfd(sp,10);
		fcntl(fd,F_SETFD,FD_CLOEXEC);
	}
	if(fd < 3)
		return;
	if(flag==SFIO_NEW)
	{
		if(!sh.sftable[fd] && sh.fdstatus[fd]==IOCLOSE)
		{
			sh.sftable[fd] = sp;
			flag = (mode&SFIO_WRITE)?IOWRITE:0;
			if(mode&SFIO_READ)
				flag |= IOREAD;
			sh.fdstatus[fd] = flag;
			sh_iostream(fd);
		}
		if((pp=(struct checkpt*)sh.jmplist) && pp->mode==SH_JMPCMD)
		{
			struct openlist *item;
			/*
			 * record open file descriptors so they can
			 * be closed in case a longjmp prevents
			 * built-ins from cleanup
			 */
			item = new_of(struct openlist, 0);
			item->strm = sp;
			item->next = pp->olist;
			pp->olist = item;
		}
		if(fdnotify)
			(*fdnotify)(-1,sffileno(sp));
	}
	else if(flag==SFIO_CLOSING || (flag==SFIO_SETFD  && newfd<=2))
	{
		sh.sftable[fd] = 0;
		sh.fdstatus[fd]=IOCLOSE;
		if(pp=(struct checkpt*)sh.jmplist)
		{
			struct openlist *item;
			for(item=pp->olist; item; item=item->next)
			{
				if(item->strm == sp)
				{
					item->strm = 0;
					break;
				}
			}
		}
	}
}

struct eval
{
	Sfdisc_t	disc;
	char		**argv;
	int		slen;
	char		addspace;
};

/*
 * Create a stream consisting of a space separated argv[] list
 */
Sfio_t *sh_sfeval(char *argv[])
{
	Sfio_t *iop;
	char *cp;
	if(argv[1])
		cp = "";
	else
		cp = argv[0];
	iop = sfopen(NULL,(char*)cp,"s");
	if(argv[1])
	{
		struct eval *ep;
		ep = new_of(struct eval,0);
		ep->disc = eval_disc;
		ep->argv = argv;
		ep->slen  = -1;
		ep->addspace  = 0;
		sfdisc(iop,&ep->disc);
	}
	return iop;
}

/*
 * This code gets called whenever an end of string is found with eval
 */
static int eval_exceptf(Sfio_t *iop,int type, void *data, Sfdisc_t *handle)
{
	struct eval *ep = (struct eval*)handle;
	char	*cp;
	int	len;
	NOT_USED(data);
	/* no more to do */
	if(type!=SFIO_READ || !(cp = ep->argv[0]))
	{
		if(type==SFIO_CLOSING)
			sfdisc(iop,SFIO_POPDISC);
		else if(ep && (type==SFIO_DPOP || type==SFIO_FINAL))
			free(ep);
		return 0;
	}

	if(!ep->addspace)
	{
		/* get the length of this string */
		ep->slen = len = (int)strlen(cp);
		/* move to next string */
		ep->argv++;
	}
	else /* insert space between arguments */
	{
		len = 1;
		cp = " ";
	}
	/* insert the new string */
	sfsetbuf(iop,cp,len);
	ep->addspace = !ep->addspace;
	return 1;
}

/*
 * This routine returns a stream pointer to a segment of length <size> from
 * the stream <sp> starting at offset <offset>
 * The stream can be read with the normal stream operations
 */
static Sfio_t *subopen(Sfio_t* sp, off_t offset, long size)
{
	struct subfile *disp;
	if(sfseek(sp,offset,SEEK_SET) <0)
		return NULL;
	disp = (struct subfile*)sh_malloc(sizeof(struct subfile)+IOBSIZE+1);
	disp->disc = sub_disc;
	disp->oldsp = sp;
	disp->offset = offset;
	disp->size = disp->left = size;
	sp = sfnew(NULL,(char*)(disp+1),IOBSIZE,PSEUDOFD,SFIO_READ);
	sfdisc(sp,&disp->disc);
	return sp;
}

/*
 * read function for subfile discipline
 */
static ssize_t subread(Sfio_t* sp,void* buff,size_t size,Sfdisc_t* handle)
{
	struct subfile *disp = (struct subfile*)handle;
	ssize_t	n;
	NOT_USED(sp);
	sfseek(disp->oldsp,disp->offset,SEEK_SET);
	if(disp->left == 0)
		return 0;
	if(size > disp->left)
		size = disp->left;
	disp->left -= size;
	n = sfread(disp->oldsp,buff,size);
	if(size>0)
		disp->offset += size;
	return n;
}

/*
 * exception handler for subfile discipline
 */
static int subexcept(Sfio_t* sp,int mode, void *data, Sfdisc_t* handle)
{
	struct subfile *disp = (struct subfile*)handle;
	NOT_USED(data);
	if(mode==SFIO_CLOSING)
	{
		sfdisc(sp,SFIO_POPDISC);
		sfsetfd(sp,-1);
		return 0;
	}
	else if(disp && (mode==SFIO_DPOP || mode==SFIO_FINAL))
	{
		free(disp);
		return 0;
	}
	else if (mode==SFIO_ATEXIT)
	{
		sfdisc(sp, SFIO_POPDISC);
		return 0;
	}
	else if(mode==SFIO_READ)
		return 0;
	return -1;
}

#define NROW    15      /* number of rows before going to multi-columns */
#define LBLSIZ	3	/* size of label field and interfield spacing */
/*
 * print a list of arguments in columns
 */
void	sh_menu(Sfio_t *outfile,int argn,char *argv[])
{
	int i,j;
	char **arg;
	int nrow, ncol=1, ndigits=1;
	int fldsize, wsize = ed_window();
	sh_winsize(&nrow,NULL);
	nrow = nrow ? (2 * (nrow / 3) + 1) : NROW;
	for(i=argn;i >= 10;i /= 10)
		ndigits++;
	if(argn < nrow)
	{
		nrow = argn;
		goto skip;
	}
	i = 0;
	for(arg=argv; *arg;arg++)
	{
		if((j=strlen(*arg)) > i)
			i = j;
	}
	i += (ndigits+LBLSIZ);
	if(i < wsize)
		ncol = wsize/i;
	if(argn > nrow*ncol)
	{
		nrow = 1 + (argn-1)/ncol;
	}
	else
	{
		ncol = 1 + (argn-1)/nrow;
		nrow = 1 + (argn-1)/ncol;
	}
skip:
	fldsize = (wsize/ncol)-(ndigits+LBLSIZ);
	for(i=0;i<nrow;i++)
	{
		if(sh.trapnote&SH_SIGSET)
			return;
		j = i;
		while(1)
		{
			arg = argv+j;
			sfprintf(outfile,"%*d) %s",ndigits,j+1,*arg);
			j += nrow;
			if(j >= argn)
				break;
			sfnputc(outfile,' ',fldsize-strlen(*arg));
		}
		sfputc(outfile,'\n');
	}
}

#undef read
/*
 * shell version of read() for user added builtins
 */
ssize_t sh_read(int fd, void* buff, size_t n)
{
	Sfio_t *sp;
	if(sp=sh.sftable[fd])
		return sfread(sp,buff,n);
	else
		return read(fd,buff,n);
}

#undef write
/*
 * shell version of write() for user added builtins
 */
ssize_t sh_write(int fd, const void* buff, size_t n)
{
	Sfio_t *sp;
	if(sp=sh.sftable[fd])
		return sfwrite(sp,buff,n);
	else
		return write(fd,buff,n);
}

#undef lseek
/*
 * shell version of lseek() for user added builtins
 */
off_t sh_seek(int fd, off_t offset, int whence)
{
	Sfio_t *sp;
	if((sp=sh.sftable[fd]) && (sfset(sp,0,0)&(SFIO_READ|SFIO_WRITE)))
		return sfseek(sp,offset,whence);
	else
		return lseek(fd,offset,whence);
}

#undef dup
int sh_dup(int old)
{
	int fd = dup(old);
	if(fd>=0)
	{
		if(sh.fdstatus[old] == IOCLOSE)
			sh.fdstatus[old] = 0;
		sh.fdstatus[fd] = (sh.fdstatus[old]&~IOCLEX);
		if(fdnotify)
			(*fdnotify)(old,fd);
	}
	return fd;
}

#undef fcntl
int sh_fcntl(int fd, int op, ...)
{
	int newfd, arg;
	va_list		ap;
	va_start(ap, op);
	arg =  va_arg(ap, int) ;
	va_end(ap);
	newfd = fcntl(fd,op,arg);
	if(newfd>=0) switch(op)
	{
	    case F_DUPFD:
		if(sh.fdstatus[fd] == IOCLOSE)
			sh.fdstatus[fd] = 0;
		if(newfd>=sh.lim.open_max)
			sh_iovalidfd(newfd);
		sh.fdstatus[newfd] = (sh.fdstatus[fd]&~IOCLEX);
		if(fdnotify)
			(*fdnotify)(fd,newfd);
		break;
	    case F_SETFD:
		if(sh.fdstatus[fd] == IOCLOSE)
			sh.fdstatus[fd] = 0;
		if(arg&FD_CLOEXEC)
			sh.fdstatus[fd] |= IOCLEX;
		else
			sh.fdstatus[fd] &= ~IOCLEX;
	}
	return newfd;
}

#undef umask
mode_t	sh_umask(mode_t m)
{
	sh.mask = m;
	return umask(m);
}

/*
 * give file descriptor <fd> and <mode>, return an iostream pointer
 * <mode> must be SFIO_READ or SFIO_WRITE
 * <fd> must be a non-negative number ofr SH_IOCOPROCESS or SH_IOHISTFILE.
 * returns NULL on failure and may set errno.
 */
Sfio_t *sh_iogetiop(int fd, int mode)
{
	int n;
	Sfio_t *iop=0;
	if(mode!=SFIO_READ && mode!=SFIO_WRITE)
	{
		errno = EINVAL;
		return iop;
	}
	switch(fd)
	{
	    case SH_IOHISTFILE:
		if(!sh_histinit())
			return iop;
		fd = sffileno(sh.hist_ptr->histfp);
		break;
	    case SH_IOCOPROCESS:
		if(mode==SFIO_WRITE)
			fd = sh.coutpipe;
		else
			fd = sh.cpipe[0];
		break;
	    default:
		if(fd<0 || !sh_iovalidfd(fd))
			fd = -1;
	}
	if(fd<0)
	{
		errno = EBADF;
		return iop;
	}
	if(!(n=sh.fdstatus[fd]))
		n = sh_iocheckfd(fd);
	if(mode==SFIO_WRITE && !(n&IOWRITE))
		return iop;
	if(mode==SFIO_READ && !(n&IOREAD))
		return iop;
	if(!(iop = sh.sftable[fd]))
		iop=sh_iostream(fd);
	return iop;
}

typedef int (*Notify_f)(int,int);

Notify_f    sh_fdnotify(Notify_f notify)
{
	Notify_f old;
	old = fdnotify;
	fdnotify = notify;
	return old;
}

Sfio_t	*sh_fd2sfio(int fd)
{
	int status;
	Sfio_t *sp = sh.sftable[fd];
	if(!sp  && (status = sh_iocheckfd(fd))!=IOCLOSE)
	{
		int flags=0;
		if(status&IOREAD)
			flags |= SFIO_READ;
		if(status&IOWRITE)
			flags |= SFIO_WRITE;
		sp = sfnew(NULL, NULL, -1, fd,flags);
		sh.sftable[fd] = sp;
	}
	return sp;
}

Sfio_t *sh_pathopen(const char *cp)
{
	int n;
	if((n=path_open(cp,path_get(cp))) < 0)
		n = path_open(cp,NULL);
	if(n < 0)
	{
		errormsg(SH_DICT,ERROR_system(1),e_open,cp);
		UNREACHABLE();
	}
	return sh_iostream(n);
}

int sh_isdevfd(const char *fd)
{
	if(!fd || strncmp(fd,"/dev/fd/",8) || fd[8]==0)
		return 0;
	for ( fd=&fd[8] ; *fd != '\0' ; fd++ )
	{
		if (*fd < '0' || *fd > '9')
			return 0;
	}
	return 1;
}

#undef fchdir
int sh_fchdir(int fd)
{
	int r,err=errno;
	while((r=fchdir(fd))<0 && errno==EINTR)
		errno = err;
	return r;
}

#undef chdir
int sh_chdir(const char* dir)
{
	int r,err=errno;
	while((r=chdir(dir))<0 && errno==EINTR)
		errno = err;
	return r;
}
