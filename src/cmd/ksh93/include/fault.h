/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1982-2011 AT&T Intellectual Property          *
*          Copyright (c) 2020-2024 Contributors to ksh 93u+m           *
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
*                                                                      *
***********************************************************************/
#ifndef SH_SIGBITS
/*
 *	UNIX shell
 *	S. R. Bourne
 *	Rewritten by David Korn
 *
 */

#include	<sig.h>
#include	<setjmp.h>
#include	<error.h>
#include	<sfio.h>


#ifndef SIGWINCH
#   ifdef SIGWIND
#	define SIGWINCH	SIGWIND
#   else
#	ifdef SIGWINDOW
#	    define SIGWINCH	SIGWINDOW
#	endif
#   endif
#endif

typedef void (*SH_SIGTYPE)(int,void(*)(int));

#define SH_FORKLIM		16	/* fork timeout interval */

#define SH_TRAP			0200	/* bit for internal traps */
#define SH_ERRTRAP		0	/* trap for non-zero exit status */
#define SH_KEYTRAP		1	/* trap for keyboard event */
#define SH_DEBUGTRAP		2	/* must be last internal trap */

#define SH_SIGBITS		8
#define SH_SIGFAULT		1	/* signal handler is sh_fault */
#define SH_SIGOFF		2	/* signal handler is SIG_IGN */
#define SH_SIGSET		4	/* pending signal */
#define SH_SIGTRAP		010	/* pending trap */
#define SH_SIGDONE		020	/* default is exit */
#define SH_SIGIGNORE		040	/* default is ignore signal */
#define SH_SIGINTERACTIVE	0100	/* handle interactive specially */
#define SH_SIGTSTP		0200	/* tstp signal received */
#define SH_SIGALRM		0200	/* timer alarm received */
#define SH_SIGTERM		SH_SIGOFF /* term signal received */
#define SH_SIGRUNTIME		0400	/* runtime value */

#define SH_SIGRTMIN		0	/* sh.sigruntime[] index */
#define SH_SIGRTMAX		1	/* sh.sigruntime[] index */

/*
 * These are longjmp values
 */

#define SH_JMPDOT	2
#define SH_JMPEVAL	3
#define SH_JMPTRAP	4
#define SH_JMPIO	5
#define SH_JMPCMD	6
#define SH_JMPFUN	7
#define SH_JMPERRFN	8
#define SH_JMPSUB	9
#define SH_JMPERREXIT	10
#define SH_JMPEXIT	11
#define SH_JMPSCRIPT	12

struct openlist
{
	Sfio_t	*strm;
	struct openlist *next;
};

struct checkpt
{
	sigjmp_buf	buff;
	sigjmp_buf	*prev;
	int		topfd;
	int		mode;
	struct openlist	*olist;
	Error_context_t err;
};

#define sh_pushcontext(bp,n) \
( \
	(bp)->mode = (n), \
	(bp)->olist = 0, \
	(bp)->topfd = sh.topfd, \
	(bp)->prev = sh.jmplist, \
	(bp)->err = *ERROR_CONTEXT_BASE, \
	sh.jmplist = (sigjmp_buf*)(&(bp)->buff) \
)
#define sh_popcontext(bp) \
( \
	sh.jmplist = (bp)->prev, \
	errorpop(&((bp)->err)) \
)

/* signal handling shorthands */
#define sh_sigaction(s,action) \
do { \
	sigset_t ss; \
	sigemptyset(&ss); \
	if(s) \
		sigaddset(&ss,(s)); \
	sigprocmask(action,&ss,0); \
} while(0)
#define sigrelease(s)	sh_sigaction(s,SIG_UNBLOCK)
#define sigblock(s)	sh_sigaction(s,SIG_BLOCK)
#define sig_begin()	sh_sigaction(0,SIG_SETMASK)

extern noreturn void 	sh_done(int);
extern void 	sh_fault(int);
extern void	sh_winsize(int*,int*);
extern void 	sh_sigclear(int);
extern void 	sh_sigdone(void);
extern void	sh_siginit(void);
extern void 	sh_sigtrap(int);
extern void 	sh_sigreset(int);
extern void 	*sh_timeradd(Sfulong_t,int ,void (*)(void*),void*);
extern void	sh_timerdel(void*);

extern const char e_alarm[];

#endif /* !SH_SIGBITS */
