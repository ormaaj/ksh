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
*            Johnothan King <johnothanking@protonmail.com>             *
*                                                                      *
***********************************************************************/
/*
 * ulimit [-HSaMctdfkxlqenVuPpmrRbiswTv] [limit]
 *
 *   David Korn
 *   AT&T Labs
 *
 */

#include	"shopt.h"
#include	<ast.h>
#include	<sfio.h>
#include	<error.h>
#include	"defs.h"
#include	"builtins.h"
#include	"name.h"
#include	"ulimit.h"
#ifndef SH_DICT
#   define SH_DICT	"libshell"
#endif

#ifdef _no_ulimit
	int	b_ulimit(int argc,char *argv[],Shbltin_t *context)
	{
		NOT_USED(argc);
		NOT_USED(argv);
		NOT_USED(context);
		errormsg(SH_DICT,ERROR_exit(2),e_nosupport);
		UNREACHABLE();
	}
#else

static int infof(Opt_t* op, Sfio_t* sp, const char* s, Optdisc_t* dp)
{
	const Limit_t*	tp;

	NOT_USED(op);
	NOT_USED(s);
	NOT_USED(dp);
	for (tp = shtab_limits; tp->option; tp++)
	{
		sfprintf(sp, "[%c=%d:%s?The %s", tp->option, tp - shtab_limits + 1, tp->name, tp->description);
		if(tp->type != LIM_COUNT)
			sfprintf(sp, " in %ss", e_units[tp->type]);
		sfprintf(sp, ".]");
	}
	return 1;
}

#define HARD	2
#define SOFT	4

int	b_ulimit(int argc,char *argv[],Shbltin_t *context)
{
	char *limit;
	int mode=0, n;
	unsigned long hit = 0;
#if _lib_getrlimit
	struct rlimit rlp;
#endif /* _lib_getrlimit */
	const Limit_t* tp;
	char* conf;
	int label, unit, nosupport, ret=0;
	rlim_t i=0;
	char tmp[41];
	Optdisc_t disc;
	NOT_USED(context);
	memset(&disc, 0, sizeof(disc));
	disc.version = OPT_VERSION;
	disc.infof = infof;
	opt_info.disc = &disc;
	while((n = optget(argv,sh_optulimit))) switch(n)
	{
		case 'H':
			mode |= HARD;
			continue;
		case 'S':
			mode |= SOFT;
			continue;
		case 'a':
			hit = ~0;
			break;
		default:
			if(n < 0)
				hit |= (1L<<(-(n+1)));
			else
				errormsg(SH_DICT,2, e_notimp, opt_info.name);
			break;
		case ':':
			errormsg(SH_DICT,2, "%s", opt_info.arg);
			break;
		case '?':
			errormsg(SH_DICT,ERROR_usage(2), "%s", opt_info.arg);
			UNREACHABLE();
	}
	opt_info.disc = 0;
	/* default to -f */
	limit = argv[opt_info.index];
	if(hit==0)
		for(n=0; shtab_limits[n].option; n++)
			if(shtab_limits[n].index == RLIMIT_FSIZE)
			{
				hit |= (1L<<n);
				break;
			}
	/* only one option at a time for setting */
	label = (hit&(hit-1));
	if(error_info.errors || (limit && label) || argc>opt_info.index+1)
	{
		errormsg(SH_DICT,ERROR_usage(2),optusage(NULL));
		UNREACHABLE();
	}
	if(mode==0)
		mode = (HARD|SOFT);
	for(tp = shtab_limits; tp->option && hit; tp++,hit>>=1)
	{
		if(!(hit&1))
			continue;
		nosupport = (n = tp->index) == RLIMIT_UNKNOWN;
		unit = shtab_units[tp->type];
		if(limit)
		{
			if(sh.subshell && !sh.subshare)
				sh_subfork();
			if(strcmp(limit,e_unlimited)==0)
				i = ULIMIT_INFINITY;
			else
			{
				char *last;
				/* an explicit suffix unit overrides the default */
				if((i=strtol(limit,&last,0))!=ULIMIT_INFINITY && !*last)
					i *= unit;
				else if((i=strton(limit,&last,NULL,0))==ULIMIT_INFINITY || *last)
				{
					if((i=sh_strnum(limit,&last,2))==ULIMIT_INFINITY || *last)
					{
						errormsg(SH_DICT,ERROR_system(1),e_number,limit);
						UNREACHABLE();
					}
					i *= unit;
				}
			}
			if(nosupport)
			{
				errormsg(SH_DICT,ERROR_system(1),e_readonly,tp->name);
				UNREACHABLE();
			}
			else
			{
#if _lib_getrlimit
				if(getrlimit(n,&rlp) <0)
				{
					errormsg(SH_DICT,ERROR_system(1),e_number,limit);
					UNREACHABLE();
				}
				if(mode&HARD)
					rlp.rlim_max = i;
				if(mode&SOFT)
					rlp.rlim_cur = i;
				if(setrlimit(n,&rlp) <0)
				{
					errormsg(SH_DICT,ERROR_system(1),e_overlimit,limit);
					UNREACHABLE();
				}
#else
				if((i=vlimit(n,i)) < 0)
				{
					errormsg(SH_DICT,ERROR_system(1),e_number,limit);
					UNREACHABLE();
				}
#endif /* _lib_getrlimit */
			}
		}
		else
		{
			if(!nosupport)
			{
#if _lib_getrlimit
				if(getrlimit(n,&rlp)<0)
				{
					errormsg(SH_DICT,ERROR_system(0),e_limit,tp->description);
					ret++;
					continue;
				}
				if(mode&HARD)
					i = rlp.rlim_max;
				if(mode&SOFT)
					i = rlp.rlim_cur;
#else
#   if _lib_ulimit
				n--;
#   endif /* _lib_ulimit */
				i = -1;
				if((i=vlimit(n,i)) < 0)
				{
					errormsg(SH_DICT,ERROR_system(0),e_limit,tp->description);
					ret++;
					continue;
				}
#endif /* _lib_getrlimit */
			}
			if(label)
			{
				if(tp->type != LIM_COUNT)
					sfsprintf(tmp,sizeof(tmp),"%s (%ss)", tp->description, e_units[tp->type]);
				else
					sfsprintf(tmp,sizeof(tmp),"%s", tp->name);
				sfprintf(sfstdout,"%-39s (-%c)  ",tmp,tp->option);
			}
			if(nosupport)
			{
				if(!tp->conf || !*(conf = astconf(tp->conf, NULL, NULL)))
					conf = (char*)e_nosupport;
				sfputr(sfstdout,conf,'\n');
			}
			else if(i!=ULIMIT_INFINITY)
			{
				i += (unit-1);
				sfprintf(sfstdout,"%I*d\n",sizeof(i),i/unit);
			}
			else
				sfputr(sfstdout,e_unlimited,'\n');
		}
	}
	return ret;
}
#endif /* _no_ulimit */
