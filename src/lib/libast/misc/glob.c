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
*            Johnothan King <johnothanking@protonmail.com>             *
*                                                                      *
***********************************************************************/

/*
 * file name expansion - POSIX.2 glob with GNU and AST extensions
 *
 *	David Korn
 *	Glenn Fowler
 *	AT&T Research
 */

#include <ast.h>
#include <ls.h>
#include <stk.h>
#include <ast_dir.h>
#include <error.h>
#include <ctype.h>
#include <regex.h>

/*
 * GLOB_MAGIC is used for sanity checking. Its significant bits must not overlap with those used
 * for flags. If a new GLOB_* flag bit is added to glob.h, these must be adapted accordingly.
 */
#define GLOB_MAGIC	0xAAA80000	/* 10101010101010000000000000000000 */
#define GLOB_FLAGMASK	0x0007FFFF	/* 00000000000001111111111111111111 */

#define MATCH_RAW	1
#define MATCH_MAKE	2
#define MATCH_META	4

#define MATCHPATH(g)	(offsetof(globlist_t,gl_path)+(g)->gl_extra)

typedef int (*GL_error_f)(const char*, int);
typedef void* (*GL_opendir_f)(const char*);
typedef struct dirent* (*GL_readdir_f)(void*);
typedef void (*GL_closedir_f)(void*);
typedef int (*GL_stat_f)(const char*, struct stat*);

#define _GLOB_PRIVATE_ \
	GL_error_f	gl_errfn; \
	int		gl_error; \
	char*		gl_nextpath; \
	globlist_t*	gl_rescan; \
	globlist_t*	gl_match; \
	Stk_t*		gl_stak; \
	int		re_flags; \
	int		re_first; \
	regex_t*	gl_ignore; \
	regex_t*	gl_ignorei; \
	regex_t		re_ignore; \
	regex_t		re_ignorei; \
	unsigned long	gl_starstar; \
	char*		gl_opt; \
	char*		gl_pat; \
	char*		gl_pad[4];

#include <glob.h>

static Stk_t *globstk = stkstd;

/*
 * default gl_diropen
 */

static void*
gl_diropen(glob_t* gp, const char* path)
{
	return (*gp->gl_opendir)(path);
}

/*
 * default gl_dirnext
 */

static char*
gl_dirnext(glob_t* gp, void* handle)
{
	struct dirent*	dp;

	while (dp = (struct dirent*)(*gp->gl_readdir)(handle))
	{
#ifdef D_TYPE
		if (D_TYPE(dp) != DT_UNKNOWN && D_TYPE(dp) != DT_DIR && D_TYPE(dp) != DT_LNK)
			gp->gl_status |= GLOB_NOTDIR;
#endif
		return dp->d_name;
	}
	return NULL;
}

/*
 * default gl_dirclose
 */

static void
gl_dirclose(glob_t* gp, void* handle)
{
	(gp->gl_closedir)(handle);
}

/*
 * default gl_type
 */

static int
gl_type(glob_t* gp, const char* path, int flags)
{
	int		type;
	struct stat	st;

	if ((flags & GLOB_STARSTAR) ? (*gp->gl_lstat)(path, &st) : (*gp->gl_stat)(path, &st))
		type = 0;
	else if (S_ISDIR(st.st_mode))
		type = GLOB_DIR;
	else if (S_ISLNK(st.st_mode))
		type = GLOB_SYM;
	else if (!S_ISREG(st.st_mode))
		type = GLOB_DEV;
	else if (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))
		type = GLOB_EXE;
	else
		type = GLOB_REG;
	return type;
}

/*
 * default gl_attr
 */

static int
gl_attr(glob_t* gp, const char* path, int flags)
{
	NOT_USED(gp);
	NOT_USED(flags);
	return pathicase(path) > 0 ? GLOB_ICASE : 0;
}

/*
 * default gl_nextdir
 */

static char*
gl_nextdir(glob_t* gp, char* dir)
{
	if (!(dir = gp->gl_nextpath))
		dir = gp->gl_nextpath = stkcopy(globstk,pathbin());
	switch (*gp->gl_nextpath)
	{
	case 0:
		dir = 0;
		break;
	case ':':
		while (*gp->gl_nextpath == ':')
			gp->gl_nextpath++;
		dir = ".";
		break;
	default:
		while (*gp->gl_nextpath)
			if (*gp->gl_nextpath++ == ':')
			{
				*(gp->gl_nextpath - 1) = 0;
				break;
			}
		break;
	}
	return dir;
}

/*
 * error intercept
 */

static int
errorcheck(glob_t* gp, const char* path)
{
	int	r = 1;

	if (gp->gl_errfn)
		r = (*gp->gl_errfn)(path, errno);
	if (gp->gl_flags & GLOB_ERR)
		r = 0;
	if (!r)
		gp->gl_error = GLOB_ABORTED;
	return r;
}

/*
 * remove backslashes
 */

static void
trim(char* sp, char* p1, int* n1, char* p2, int* n2)
{
	char*	dp = sp;
	int	c;

	if (p1)
		*n1 = 0;
	if (p2)
		*n2 = 0;
	do
	{
		if ((c = *sp++) == '\\')
			c = *sp++;
		if (sp == p1)
		{
			p1 = 0;
			*n1 = sp - dp - 1;
		}
		if (sp == p2)
		{
			p2 = 0;
			*n2 = sp - dp - 1;
		}
	} while (*dp++ = c);
}

static void
addmatch(glob_t* gp, const char* dir, const char* pat, const char* rescan, char* endslash, int meta)
{
	globlist_t*	ap;
	int		offset;
	int		type;

	stkseek(globstk,MATCHPATH(gp));
	if (dir)
	{
		sfputr(globstk,dir,-1);
		sfputc(globstk,gp->gl_delim);
	}
	if (endslash)
		*endslash = 0;
	sfputr(globstk,pat,-1);
	if (rescan)
	{
		if ((*gp->gl_type)(gp, stkptr(globstk,MATCHPATH(gp)), 0) != GLOB_DIR)
			return;
		sfputc(globstk,gp->gl_delim);
		offset = stktell(globstk);
		/* if null, reserve room for . */
		if (*rescan)
			sfputr(globstk,rescan,-1);
		else
			sfputc(globstk,0);
		sfputc(globstk,0);
		rescan = stkptr(globstk,offset);
		ap = stkfreeze(globstk,0);
		ap->gl_begin = (char*)rescan;
		ap->gl_next = gp->gl_rescan;
		gp->gl_rescan = ap;
	}
	else
	{
		if (!endslash && (gp->gl_flags & GLOB_MARK) && (type = (*gp->gl_type)(gp, stkptr(globstk,MATCHPATH(gp)), 0)))
		{
			if ((gp->gl_flags & GLOB_COMPLETE) && type != GLOB_EXE)
			{
				stkseek(globstk,0);
				return;
			}
			else if (type == GLOB_DIR && (gp->gl_flags & GLOB_MARK))
				sfputc(globstk,gp->gl_delim);
		}
		ap = stkfreeze(globstk,1);
		ap->gl_next = gp->gl_match;
		gp->gl_match = ap;
		gp->gl_pathc++;
	}
	ap->gl_flags = MATCH_RAW|meta;
	if (gp->gl_flags & GLOB_COMPLETE)
		ap->gl_flags |= MATCH_MAKE;
}

/*
 * this routine builds a list of files that match a given pathname
 * uses REG_SHELL of <regex> to match each component
 * a leading . must match explicitly
 */

static void
glob_dir(glob_t* gp, globlist_t* ap, int re_flags)
{
	char*		rescan;
	char*		prefix;
	char*		pat;
	char*		name;
	int		c;
	char*		dirname;
	void*		dirf;
	char		first;
	regex_t*	ire;
	regex_t*	pre;
	regex_t		rec;
	regex_t		rei;
	int		notdir;
	int		t1;
	int		t2;
	int		bracket;

	int		anymeta = ap->gl_flags & MATCH_META;
	int		complete = 0;
	int		err = 0;
	int		meta = ((gp->re_flags & REG_ICASE) && *ap->gl_begin != '/') ? MATCH_META : 0;
	int		quote = 0;
	int		savequote = 0;
	char*		restore1 = 0;
	char*		restore2 = 0;
	regex_t*	prec = 0;
	regex_t*	prei = 0;
	char*		matchdir = 0;
	int		starstar = 0;

	if (*gp->gl_intr)
	{
		gp->gl_error = GLOB_INTR;
		return;
	}
	pat = rescan = ap->gl_begin;
	prefix = dirname = ap->gl_path + gp->gl_extra;
	first = (rescan == prefix);
again:
	bracket = 0;
	for (;;)
	{
		switch (c = *rescan++)
		{
		case 0:
			if (meta)
			{
				rescan = 0;
				break;
			}
			if (quote)
			{
				trim(ap->gl_begin, rescan, &t1, NULL, NULL);
				rescan -= t1;
			}
			if (!first && !*rescan && *(rescan - 2) == gp->gl_delim)
			{
				*(rescan - 2) = 0;
				c = (*gp->gl_type)(gp, prefix, 0);
				*(rescan - 2) = gp->gl_delim;
				if (c == GLOB_DIR)
					addmatch(gp, NULL, prefix, NULL, rescan - 1, anymeta);
			}
			else if ((anymeta || !(gp->gl_flags & GLOB_NOCHECK)) && (*gp->gl_type)(gp, prefix, 0))
				addmatch(gp, NULL, prefix, NULL, NULL, anymeta);
			return;
		case '[':
			if (!bracket)
			{
				bracket = MATCH_META;
				if (*rescan == '!' || *rescan == '^')
					rescan++;
				if (*rescan == ']')
					rescan++;
			}
			continue;
		case ']':
			meta |= bracket;
			continue;
		case '(':
			if (!(gp->gl_flags & GLOB_AUGMENTED))
				continue;
			/* FALLTHROUGH */
		case '*':
		case '?':
			meta = MATCH_META;
			continue;
		case '\\':
			if (!(gp->gl_flags & GLOB_NOESCAPE))
			{
				quote = 1;
				if (*rescan)
					rescan++;
			}
			continue;
		default:
			if (c == gp->gl_delim)
			{
				if (meta)
					break;
				pat = rescan;
				bracket = 0;
				savequote = quote;
			}
			continue;
		}
		break;
	}
	anymeta |= meta;
	if (matchdir)
		goto skip;
	if (pat == prefix)
	{
		prefix = 0;
		if (!rescan && (gp->gl_flags & GLOB_COMPLETE))
		{
			complete = 1;
			dirname = 0;
		}
		else
			dirname = ".";
	}
	else
	{
		if (pat == prefix + 1)
			dirname = "/";
		if (savequote)
		{
			quote = 0;
			trim(ap->gl_begin, pat, &t1, rescan, &t2);
			pat -= t1;
			if (rescan)
				rescan -= t2;
		}
		*(restore1 = pat - 1) = 0;
	}
	if (!complete && (gp->gl_flags & GLOB_STARSTAR))
		while (pat[0] == '*' && pat[1] == '*' && (pat[2] == '/'  || pat[2]==0))
		{
			matchdir = pat;
			if (pat[2])
			{
				pat += 3;
				while (*pat=='/')
					pat++;
				if (*pat)
					continue;
			}
			rescan = *pat?0:pat;
			pat = "*";
			goto skip;
		}
	if (matchdir)
	{
		rescan = pat;
		goto again;
	}
skip:
	if (rescan)
		*(restore2 = rescan - 1) = 0;
	if (rescan && !complete && (gp->gl_flags & GLOB_STARSTAR))
	{
		char*	p = rescan;

		while (p[0] == '*' && p[1] == '*' && (p[2] == '/'  || p[2]==0))
		{
			rescan = p;
			if (starstar = (p[2]==0))
				break;
			p += 3;
			while (*p=='/')
				p++;
			if (*p==0)
			{
				starstar = 2;
				break;
			}
		}
	}
	if (matchdir)
		gp->gl_starstar++;
	if (gp->gl_opt)
		pat = strcpy(gp->gl_opt, pat);
	for (;;)
	{
		if (complete)
		{
			if (!(dirname = (*gp->gl_nextdir)(gp, dirname)))
				break;
			prefix = streq(dirname, ".") ? NULL : dirname;
		}
		if ((!starstar && !gp->gl_starstar || (t1 = (*gp->gl_type)(gp, dirname, GLOB_STARSTAR)) == GLOB_DIR
			|| t1 == GLOB_SYM && pat[0]=='*' && pat[1]=='\0') /* follow symlinks to dirs for non-globstar components */
		&& (dirf = (*gp->gl_diropen)(gp, dirname)))
		{
			if (!(gp->re_flags & REG_ICASE)
			&& (gp->gl_flags & GLOB_DCASE)
			&& ((*gp->gl_attr)(gp, dirname, 0) & GLOB_ICASE))
			{
				if (!prei)
				{
					if (err = regcomp(&rei, pat, gp->re_flags|REG_ICASE))
						break;
					prei = &rei;
					if (gp->re_first)
					{
						gp->re_first = 0;
						gp->re_flags = regstat(prei)->re_flags & ~REG_ICASE;
					}
				}
				pre = prei;
			}
			else
			{
				if (!prec)
				{
					if (err = regcomp(&rec, pat, gp->re_flags))
						break;
					prec = &rec;
					if (gp->re_first)
					{
						gp->re_first = 0;
						gp->re_flags = regstat(prec)->re_flags;
					}
				}
				pre = prec;
			}
			if ((ire = gp->gl_ignore) && (gp->re_flags & REG_ICASE))
			{
				if (!gp->gl_ignorei)
				{
					if (regcomp(&gp->re_ignorei, gp->gl_fignore, re_flags|REG_ICASE))
					{
						gp->gl_error = GLOB_APPERR;
						break;
					}
					gp->gl_ignorei = &gp->re_ignorei;
				}
				ire = gp->gl_ignorei;
			}
			if (restore2)
				*restore2 = gp->gl_delim;
			while ((name = (*gp->gl_dirnext)(gp, dirf)) && !*gp->gl_intr)
			{
				/*
				 * For security and usability, only match '..' or '.' as the final element if:
				 *	- it's specified literally, or
				 *	- we're in file name completion mode.
				 * To avoid breaking globstar, make sure that '.' or '..' is skipped if, and only if, it is the
				 * final element in the pattern (i.e., if 'pat' does not contain a slash) and is not specified
				 * literally as '.' or '..', i.e. only if '.' or '..' was actually resolved from a glob pattern.
				 */
				if (!(matchdir && (pat[0] == '.' && (!pat[1] || pat[1] == '.' && !pat[2]) || strchr(pat,'/')))
				&& name[0] == '.' && (!name[1] || name[1] == '.' && !name[2])
				&& !(gp->gl_flags & GLOB_FCOMPLETE))
					continue;
				if (notdir = (gp->gl_status & GLOB_NOTDIR))
					gp->gl_status &= ~GLOB_NOTDIR;
				if (ire && !regexec(ire, name, 0, NULL, 0))
					continue;
				if (matchdir && (name[0] != '.' || name[1] && (name[1] != '.' || name[2])) && !notdir)
					addmatch(gp, prefix, name, matchdir, NULL, anymeta);
				if (!regexec(pre, name, 0, NULL, 0))
				{
					if (!rescan || !notdir)
						addmatch(gp, prefix, name, rescan, NULL, anymeta);
					if (starstar==1 || (starstar==2 && !notdir))
						addmatch(gp, prefix, name, starstar==2?"":NULL, NULL, anymeta);
				}
				errno = 0;
			}
			(*gp->gl_dirclose)(gp, dirf);
			if (err || errno && !errorcheck(gp, dirname))
				break;
		}
		else if (!complete && !errorcheck(gp, dirname))
			break;
		if (!complete)
			break;
		if (*gp->gl_intr)
		{
			gp->gl_error = GLOB_INTR;
			break;
		}
	}
	if (restore1)
		*restore1 = gp->gl_delim;
	if (restore2)
		*restore2 = gp->gl_delim;
	if (prec)
		regfree(prec);
	if (prei)
		regfree(prei);
	if (err == REG_ESPACE)
		gp->gl_error = GLOB_NOSPACE;
}

int
_ast_glob(const char* pattern, int flags, int (*errfn)(const char*, int), glob_t* gp)
{
	globlist_t*	ap;
	char*		pat;
	globlist_t*	top;
	Stk_t*		oldstak = NULL;
	char**		argv;
	char**		av;
	size_t		skip;
	unsigned long	f;
	int		n;
	int		x;
	int		re_flags;

	const char*	nocheck = pattern;
	int		optlen = 0;
	int		suflen = 0;
	int		extra = 1;
	unsigned char	intr = 0;

	gp->gl_rescan = 0;
	gp->gl_error = 0;
	gp->gl_errfn = errfn;
	if (flags & GLOB_APPEND)
	{
		if ((gp->gl_flags |= GLOB_APPEND) ^ (flags|GLOB_MAGIC))
			return GLOB_APPERR;
		if (((gp->gl_flags & GLOB_STACK) == 0) == (gp->gl_stak == 0))
			return GLOB_APPERR;
		if (gp->gl_starstar > 1)
			gp->gl_flags |= GLOB_STARSTAR;
		else
			gp->gl_starstar = 0;
	}
	else
	{
		gp->gl_flags = (flags & GLOB_FLAGMASK) | GLOB_MAGIC;
		gp->re_flags = REG_SHELL|REG_NOSUB|REG_LEFT|REG_RIGHT|((flags&GLOB_AUGMENTED)?REG_AUGMENTED:0);
		gp->gl_pathc = 0;
		gp->gl_ignore = 0;
		gp->gl_ignorei = 0;
		gp->gl_starstar = 0;
		if (!(flags & GLOB_DISC))
		{
			gp->gl_fignore = 0;
			gp->gl_suffix = 0;
			gp->gl_intr = 0;
			gp->gl_delim = 0;
			gp->gl_handle = 0;
			gp->gl_diropen = 0;
			gp->gl_dirnext = 0;
			gp->gl_dirclose = 0;
			gp->gl_type = 0;
			gp->gl_attr = 0;
			gp->gl_nextdir = 0;
			gp->gl_stat = 0;
			gp->gl_lstat = 0;
			gp->gl_extra = 0;
		}
		if (!(flags & GLOB_ALTDIRFUNC))
		{
			gp->gl_opendir = (GL_opendir_f)opendir;
			gp->gl_readdir = (GL_readdir_f)readdir;
			gp->gl_closedir = (GL_closedir_f)closedir;
			if (!gp->gl_stat)
				gp->gl_stat = (GL_stat_f)pathstat;
		}
		if (!gp->gl_lstat)
			gp->gl_lstat = (GL_stat_f)lstat;
		if (!gp->gl_intr)
			gp->gl_intr = &intr;
		if (!gp->gl_delim)
			gp->gl_delim = '/';
		if (!gp->gl_diropen)
			gp->gl_diropen = gl_diropen;
		if (!gp->gl_dirnext)
			gp->gl_dirnext = gl_dirnext;
		if (!gp->gl_dirclose)
			gp->gl_dirclose = gl_dirclose;
		if (!gp->gl_type)
			gp->gl_type = gl_type;
		if (!gp->gl_attr)
			gp->gl_attr = gl_attr;
		if (flags & GLOB_GROUP)
			gp->re_flags |= REG_SHELL_GROUP;
		if (flags & GLOB_ICASE)
			gp->re_flags |= REG_ICASE;
		if (!gp->gl_fignore)
			gp->re_flags |= REG_SHELL_DOT;
		else if (*gp->gl_fignore)
		{
			if (regcomp(&gp->re_ignore, gp->gl_fignore, gp->re_flags))
				return GLOB_APPERR;
			gp->gl_ignore = &gp->re_ignore;
		}
		if (gp->gl_flags & GLOB_STACK)
			gp->gl_stak = 0;
		else if (!(gp->gl_stak = stkopen(0)))
			return GLOB_NOSPACE;
		if ((gp->gl_flags & GLOB_COMPLETE) && !gp->gl_nextdir)
			gp->gl_nextdir = gl_nextdir;
	}
	skip = gp->gl_pathc;
	if (gp->gl_stak)
		oldstak = globstk, globstk = gp->gl_stak;
	if (flags & GLOB_DOOFFS)
		extra += gp->gl_offs;
	if (gp->gl_suffix)
		suflen =  strlen(gp->gl_suffix);
	if (*(pat = (char*)pattern) == '~' && *(pat + 1) == '(')
	{
		f = gp->gl_flags;
		n = 1;
		x = 1;
		pat += 2;
		for (;;)
		{
			switch (*pat++)
			{
			case 0:
			case ':':
				break;
			case '-':
				n = 0;
				continue;
			case '+':
				n = 1;
				continue;
			case 'i':
				if (n)
					f |= GLOB_ICASE;
				else
					f &= ~GLOB_ICASE;
				continue;
			case 'M':
				if (n)
					f |= GLOB_BRACE;
				else
					f &= ~GLOB_BRACE;
				continue;
			case 'N':
				if (n)
					f &= ~GLOB_NOCHECK;
				else
					f |= GLOB_NOCHECK;
				continue;
			case 'O':
				if (n)
					f |= GLOB_STARSTAR;
				else
					f &= ~GLOB_STARSTAR;
				continue;
			case ')':
				flags = (gp->gl_flags = f) & GLOB_FLAGMASK;
				if (f & GLOB_ICASE)
					gp->re_flags |= REG_ICASE;
				else
					gp->re_flags &= ~REG_ICASE;
				if (x)
					optlen = pat - (char*)pattern;
				break;
			default:
				x = 0;
				continue;
			}
			break;
		}
	}
	top = ap = stkalloc(globstk,(optlen ? 2 : 1) * strlen(pattern) + sizeof(globlist_t) + suflen + gp->gl_extra);
	ap->gl_next = 0;
	ap->gl_flags = 0;
	ap->gl_begin = ap->gl_path + gp->gl_extra;
	pat = strcopy(ap->gl_begin, pattern + optlen);
	if (suflen)
		pat = strcopy(pat, gp->gl_suffix);
	if (optlen)
		strlcpy(gp->gl_pat = gp->gl_opt = pat + 1, pattern, optlen);
	else
		gp->gl_pat = 0;
	suflen = 0;
	if (!(flags & GLOB_LIST))
		gp->gl_match = 0;
	re_flags = gp->re_flags;
	gp->re_first = 1;
	do
	{
		gp->gl_rescan = ap->gl_next;
		glob_dir(gp, ap, re_flags);
	} while (!gp->gl_error && (ap = gp->gl_rescan));
	gp->re_flags = re_flags;
	if (gp->gl_pathc == skip)
	{
		if (flags & GLOB_NOCHECK)
		{
			gp->gl_pathc++;
			top->gl_next = gp->gl_match;
			gp->gl_match = top;
			strcopy(top->gl_path + gp->gl_extra, nocheck);
		}
		else
			gp->gl_error = GLOB_NOMATCH;
	}
	if (flags & GLOB_LIST)
		gp->gl_list = gp->gl_match;
	else
	{
		argv = stkalloc(globstk,(gp->gl_pathc + extra) * sizeof(char*));
		if (gp->gl_flags & GLOB_APPEND)
		{
			skip += --extra;
			memcpy(argv, gp->gl_pathv, skip * sizeof(char*));
			av = argv + skip;
		}
		else
		{
			av = argv;
			while (--extra > 0)
				*av++ = 0;
		}
		gp->gl_pathv = argv;
		argv = av;
		ap = gp->gl_match;
		while (ap)
		{
			*argv++ = ap->gl_path + gp->gl_extra;
			ap = ap->gl_next;
		}
		*argv = 0;
		if (!(flags & GLOB_NOSORT) && (argv - av) > 1)
		{
			strsort(av, argv - av, strcoll);
			if (gp->gl_starstar > 1)
				av[gp->gl_pathc = struniq(av, argv - av)] = 0;
			gp->gl_starstar = 0;
		}
	}
	if (gp->gl_starstar > 1)
		gp->gl_flags &= ~GLOB_STARSTAR;
	if (gp->gl_stak)
		globstk = oldstak;
	return gp->gl_error;
}

void
_ast_globfree(glob_t* gp)
{
	if ((gp->gl_flags & GLOB_MAGIC) == GLOB_MAGIC)
	{
		gp->gl_flags &= ~GLOB_MAGIC;
		if (gp->gl_stak)
			stkclose(gp->gl_stak);
		if (gp->gl_ignore)
			regfree(gp->gl_ignore);
		if (gp->gl_ignorei)
			regfree(gp->gl_ignorei);
	}
}
