/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1982-2012 AT&T Intellectual Property          *
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
*          atheik <14833674+atheik@users.noreply.github.com>           *
*            Johnothan King <johnothanking@protonmail.com>             *
*         hyenias <58673227+hyenias@users.noreply.github.com>          *
*                                                                      *
***********************************************************************/
/*
 * KornShell lexical analyzer
 *
 * Written by David Korn
 * AT&T Labs
 *
 */

#include	"shopt.h"
#include	<ast.h>
#include	<fcin.h>
#include	<nval.h>
#include	"defs.h"
#include	"argnod.h"
#include	"test.h"
#include	"lexstates.h"
#include	"io.h"
#include	"shlex.h"

#define TEST_RE		3
#define SYNBAD		3	/* exit value for syntax errors */
#define STACK_ARRAY	3	/* size of depth match stack growth */

#if _lib_iswblank < 0	/* set in lexstates.h to enable this code */

int
local_iswblank(wchar_t wc)
{
	static int      initialized;
	static wctype_t wt;

	if (!initialized)
	{
		initialized = 1;
		wt = wctype("blank");
	}
	return iswctype(wc, wt);
}

#endif

#define pushlevel(lp,c,s)	((lp->lexd.level>=lex_max?stack_grow():1) &&\
				((lex_match[lp->lexd.level++]=lp->lexd.lastc),\
				lp->lexd.lastc=(((s)<<CHAR_BIT)|(c))))
#define oldmode(lp)	(lp->lexd.lastc>>CHAR_BIT)
#define endchar(lp)	(lp->lexd.lastc&0xff)
#define setchar(lp,c)	(lp->lexd.lastc = ((lp->lexd.lastc&~0xff)|(c)))
#define poplevel(lp)	(lp->lexd.lastc=lex_match[--lp->lexd.level])

static char		*fmttoken(Lex_t*, int);
static struct argnod	*endword(int);
static int		alias_exceptf(Sfio_t*, int, void*, Sfdisc_t*);
static void		setupalias(Lex_t*,const char*, Namval_t*);
static int		comsub(Lex_t*,int);
static void		nested_here(Lex_t*);
static int		here_copy(Lex_t*, struct ionod*);
static int 		stack_grow(void);
static const Sfdisc_t alias_disc = { NULL, NULL, NULL, alias_exceptf, NULL };

/* these were taken out of the Lex_t struct because they should never be saved and restored (see stack_grow()) */
static int		lex_max, *lex_match;

#if SHOPT_KIA

static void refvar(Lex_t *lp, int type)
{
	off_t off = (fcseek(0)-(type+1))-(lp->lexd.first?lp->lexd.first:fcfirst());
	unsigned long r;
	if(lp->lexd.first)
	{
		off = (fcseek(0)-(type+1)) - lp->lexd.first;
		r=kiaentity(lp,lp->lexd.first+kia.offset+type,off-kia.offset,'v',-1,-1,kia.current,'v',0,"");
	}
	else
	{
		int n,offset = stktell(sh.stk);
		void *savptr;
		char *begin;
		off = offset + (fcseek(0)-(type+1)) - fcfirst();
		if(kia.offset < offset)
		{
			/* variable starts on stack, copy remainder */
			if(off>offset)
				sfwrite(sh.stk,fcfirst()+type,off-offset);
			n = stktell(sh.stk)-kia.offset;
			begin = stkptr(sh.stk,kia.offset);
		}
		else
		{
			/* variable in data buffer */
			begin = fcfirst()+(type+kia.offset-offset);
			n = off-kia.offset;
		}
		savptr = stkfreeze(sh.stk,0);
		r=kiaentity(lp,begin,n,'v',-1,-1,kia.current,'v',0,"");
		stkset(sh.stk,savptr,offset);
	}
	sfprintf(kia.tmp,"p;%..64d;v;%..64d;%d;%d;r;\n",kia.current,r,sh.inlineno,sh.inlineno);
}
#endif /* SHOPT_KIA */

/*
 * This routine gets called when reading across a buffer boundary
 * If lexd.nocopy is off, then current token is saved on the stack
 */
static void lex_advance(Sfio_t *iop, const char *buff, int size, void *context)
{
	Lex_t		*lp = (Lex_t*)context;
	Sfio_t		*log= sh.funlog;
	/* write to history file and to stderr if necessary */
	if(iop && !sfstacked(iop))
	{
		if(sh_isstate(SH_HISTORY) && sh.hist_ptr)
			log = sh.hist_ptr->histfp;
		sfwrite(log, (void*)buff, size);
		if(sh_isstate(SH_VERBOSE))
			sfwrite(sfstderr, buff, size);
	}
	if(lp->lexd.nocopy)
		return;
	if(lp->lexd.dolparen && lp->lexd.docword && lp->lexd.docend)
	{
		int n = size - (lp->lexd.docend-(char*)buff);
		sfwrite(sh.strbuf,lp->lexd.docend,n);
		lp->lexd.docextra  += n;
		if(sffileno(iop)>=0)
			lp->lexd.docend = sfsetbuf(iop,iop,0);
		else
			lp->lexd.docend = fcfirst();
	}
	if(lp->lexd.first)
	{
		size -= (lp->lexd.first-(char*)buff);
		buff = lp->lexd.first;
		if(!lp->lexd.inlexskip)
			lp->arg = stkseek(sh.stk,ARGVAL);
#if SHOPT_KIA
		kia.offset += ARGVAL;
#endif /* SHOPT_KIA */
	}
	if(size>0 && (lp->arg||lp->lexd.inlexskip))
	{
		sfwrite(sh.stk,buff,size);
		lp->lexd.first = 0;
	}
}

/*
 * fill up another input buffer
 * preserves lexical state
 */
static int lexfill(Lex_t *lp)
{
	int c;
	Lex_t savelex;
	struct argnod *ap;
	int aok,docextra;
	savelex = *lp;
	ap = lp->arg;
	c = fcfill();
	if(ap)
		lp->arg = ap;
	docextra = lp->lexd.docextra;
	lp->lex = savelex.lex;
	lp->lexd = savelex.lexd;
	if(fcfile() ||  c)
		lp->lexd.first = 0;
	aok= lp->aliasok;
	ap = lp->arg;
	memcpy(lp, &savelex, offsetof(Lex_t,lexd));
	lp->arg = ap;
	lp->aliasok = aok;
	if(lp->lexd.docword && docextra)
	{
		lp->lexd.docextra = docextra;
		lp->lexd.docend = fcseek(0)-1;
	}
	return c;
}

/*
 * mode=1 for reinitialization
 */
Lex_t *sh_lexopen(Lex_t *lp, int mode)
{
	if(!lp)
		lp = sh_newof(0,Lex_t,1,0);
	else if(mode)
		lp->lex.intest = lp->lex.incase = lp->lex.skipword = lp->comp_assign = lp->comsub = lp->assignok = 0;
	else
	{	/* full reset: everything except LINENO */
		int lastline = lp->lastline, inlineno = lp->inlineno, firstline = lp->firstline;
		memset(lp,0,sizeof(Lex_t));
		lp->lastline = lastline, lp->inlineno = inlineno, lp->firstline = firstline;
	}
	lp->lex.reservok = 1;
	lp->lexd.warn = !sh_isoption(SH_DICTIONARY) && sh_isoption(SH_NOEXEC);
	fcnotify(lex_advance,lp);
	return lp;
}

#ifdef DBUG
extern int lextoken(Lex_t*);
int sh_lex(Lex_t *lp)
{
	int flag;
	char *quoted, *macro, *split, *expand;
	int tok = lextoken(lp);
	quoted = macro = split = expand = "";
	if(tok==0 && (flag=lp->arg->argflag))
	{
		if(flag&ARG_MAC)
			macro = "macro:";
		if(flag&ARG_EXP)
			expand = "expand:";
		if(flag&ARG_QUOTED)
			quoted = "quoted:";
	}
	sfprintf(sfstderr,"%lld: line %d: %o:%s%s%s%s %s\n",(Sflong_t)sh.current_pid,sh.inlineno,tok,quoted,
		macro, split, expand, fmttoken(lp,tok));
	return tok;
}
#define sh_lex	lextoken
#endif

/*
 * Get the next word and put it on the top of the stack
 * A pointer to the current word is stored in lp->arg
 * Returns the token type
 */
int sh_lex(Lex_t* lp)
{
	const char	*state;
	int		n, c, mode=ST_BEGIN, wordflags=0;
	int		inlevel=lp->lexd.level, assignment=0, ingrave=0;
	int		epatchar=0;
	char		*varnamefirst = NULL;
	int		varnamelength = 0;
	SETLEN(1);
	if(lp->lexd.paren)
	{
		lp->lexd.paren = 0;
		return lp->token=LPAREN;
	}
	if(lp->noreserv)
	{
		lp->lex.reservok = 0;
		while((c = fcgetc()) && (c==' ' || c== '\t' || c=='\n'))
			if(c=='\n')
				sh.inlineno++;
		fcseek(-LEN);
		if(c=='[')
			lp->assignok = SH_ASSIGN;
	}
	if(lp->lex.incase)
		lp->assignok = 0;
	else
		lp->assignok |= lp->lex.reservok;
	if(lp->comp_assign==2)
		lp->comp_assign = lp->lex.reservok = 0;
	lp->lexd.arith = (lp->lexd.nest==1);
	if(lp->lexd.nest)
	{
		pushlevel(lp,lp->lexd.nest,ST_NONE);
		lp->lexd.nest = 0;
		mode = lp->lexd.lex_state;
	}
	else if(lp->lexd.docword)
	{
		if((c = fcgetc())=='-' || c=='#')
		{
			lp->lexd.docword++;
			lp->digits=(c=='#'?3:1);
		}
		else if(c=='<')
		{
			lp->digits=2;
			lp->lexd.docword=0;
		}
		else if(c>0)
			fcseek(-LEN);
	}
	if(!lp->lexd.dolparen)
	{
		lp->arg = 0;
		if(mode!=ST_BEGIN)
			lp->lexd.first = fcseek(0);
		else
			lp->lexd.first = 0;
	}
	lp->lastline = sh.inlineno;
	while(1)
	{
		/* skip over characters in the current state */
		state = sh_lexstates[mode];
		while((n=STATE(state,c))==0);
		switch(n)
		{
			case S_BREAK:
				if(lp->lex.incase>TEST_RE && mode==ST_NORM && c==LPAREN)
				{
					pushlevel(lp,RPAREN,mode);
					mode = ST_NESTED;
					continue;
				}
				fcseek(-LEN);
				goto breakloop;
			case S_EOF:
				if((n=lexfill(lp)) > 0)
				{
					fcseek(-1);
					continue;
				}
				/* check for zero byte in file */
				if(n==0 && fcfile())
				{
					if(sh.readscript)
					{
						char *cp = error_info.id;
						errno = ENOEXEC;
						error_info.id = sh.readscript;
						errormsg(SH_DICT,ERROR_system(ERROR_NOEXEC),e_exec,cp);
						UNREACHABLE();
					}
					else
					{
						lp->token = -1;
						sh_syntax(lp,0);
					}
				}
				/* end-of-file */
				if(mode==ST_BEGIN)
					return lp->token=EOFSYM;
				if(mode >ST_NORM && lp->lexd.level>0 && !lp->lexd.inlexskip)
				{
					switch(c=endchar(lp))
					{
						case '$':
							if(mode==ST_LIT)
							{
								c = '\'';
								break;
							}
							mode = oldmode(lp);
							poplevel(lp);
							continue;
						case RBRACT:
							c = LBRACT;
							break;
						case 1:	/* for ((...)) */
						case RPAREN:
							c = LPAREN;
							break;
						default:
							c = LBRACE;
							break;
						case '"': case '`': case '\'':
							break;
					}
					lp->lasttok = c;
					lp->token = EOFSYM;
					sh_syntax(lp,0);
				}
				goto breakloop;
			case S_COM:
				/* skip one or more comment line(s) */
				lp->lex.reservok = !lp->lex.intest;
				if((n=lp->lexd.nocopy) && lp->lexd.dolparen)
					lp->lexd.nocopy--;
				do
				{
					while((c = fcgetc()) > 0 && c!='\n');
					if(c<=0 || lp->heredoc)
					{
						sh.inlineno++;
						break;
					}
					while(sh.inlineno++,fcpeek(0)=='\n')
						fcseek(1);
					while(state[c=fcpeek(0)]==0)
						fcseek(1);
				}
				while(c=='#');
				lp->lexd.nocopy = n;
				if(c<0)
					return lp->token=EOFSYM;
				n = S_NLTOK;
				sh.inlineno--;
				/* FALLTHROUGH */
			case S_NLTOK:
				/* check for here-document */
				if(lp->heredoc)
				{
					if(!lp->lexd.dolparen)
						lp->lexd.nocopy++;
					c = sh.inlineno;
					if(here_copy(lp,lp->heredoc)<=0 && lp->lasttok)
					{
						lp->lasttok = IODOCSYM;
						lp->token = EOFSYM;
						lp->lastline = c;
						sh_syntax(lp,0);
					}
					if(!lp->lexd.dolparen)
						lp->lexd.nocopy--;
					lp->heredoc = 0;
				}
				lp->lex.reservok = !lp->lex.intest;
				lp->lex.skipword = 0;
				/* FALLTHROUGH */
			case S_NL:
				/* skip over new-lines */
				lp->lex.last_quote = 0;
				while(sh.inlineno++,fcget()=='\n');
				fcseek(-LEN);
				if(n==S_NLTOK)
				{
					lp->comp_assign = 0;
					return lp->token='\n';
				}
				/* FALLTHROUGH */
			case S_BLNK:
				if(lp->lex.incase<=TEST_RE)
					continue;
				/* implicit RPAREN for =~ test operator */
				if(inlevel+1==lp->lexd.level)
				{
					if(lp->lex.intest)
						fcseek(-LEN);
					c = RPAREN;
					goto do_pop;
				}
				continue;
			case S_OP:
				/* return operator token */
				if(c=='<' || c=='>')
				{
					if(lp->lex.testop2)
						lp->lex.testop2 = 0;
					else
					{
						lp->digits = (c=='>');
						lp->lex.skipword = 1;
						lp->aliasok = lp->lex.reservok;
						if(lp->lex.incase<2)
							lp->lex.reservok = 0;
					}
				}
				else
				{
					lp->lex.reservok = !lp->lex.intest;
					if(c==RPAREN)
					{
						if(!lp->lexd.dolparen)
							lp->lex.incase = 0;
						return lp->token=c;
					}
					lp->lex.testop1 = lp->lex.intest;
				}
				if((n = fcgetc()) > 0)
					fcseek(-LEN);
				if(state[n]==S_OP || n=='#')
				{
					if(n==c)
					{
						if(c==LPAREN)
						{
							int r;
							/* Avoid misdetecting EXPRSYM in [[ ... ]] or compound assignments */
							if(lp->lex.intest || lp->comp_assign)
								return lp->token=c;
							/* The comsub() reading hack avoids the parser, so comp_assign is never
							 * set; try to detect compound assignments with this workaround instead */
							if(lp->lexd.dolparen && !lp->lexd.dolparen_arithexp
							&& (fcpeek(-2)=='=' || lp->lexd.dolparen_eqparen))
								return lp->token=c;
							/* OK, maybe this is EXPRSYM (arith '((', possibly following '$').
							 * But this cannot be concluded until a final '))' is detected.
							 * Use a recursive lexer invocation for that. */
							lp->lexd.nest=1;
							lp->lastline = sh.inlineno;
							lp->lexd.lex_state = ST_NESTED;
							fcseek(1);
							r = sh_lex(lp);
							if(r!=LPAREN && r!=EXPRSYM)
							{	/* throw "`(' unmatched" error */
								lp->lasttok = LPAREN;
								lp->token = EOFSYM;
								sh_syntax(lp,0);
							}
							return r;
						}
						c |= SYMREP;
						/* Here document redirection operator '<<' */
						if(c==IODOCSYM)
							lp->lexd.docword = 1;
					}
					else if(c=='(' || c==')')
						return lp->token=c;
					else if(c=='&')
					{
						if(n=='>' && !sh_isoption(SH_POSIX))
						{
							/* bash-style "&>file" shorthand for ">file 2>&1" */
							lp->digits = -1;
							c = '>';
						}
						else if(n=='|')
							c |= SYMPIPE;
						else
							n = 0;
					}
					else if(n=='&')
						c  |= SYMAMP;
					else if(c!='<' && c!='>')
						n = 0;
					else if(n==LPAREN)
					{
						/* process substitution <(...) or >(...) */
						c  |= SYMLPAR;
						lp->lex.reservok = 1;
						lp->lex.skipword = 0;
					}
					else if(n=='|')
						c  |= SYMPIPE;
					else if(c=='<' && n=='>')
					{
						lp->digits = sh_isoption(SH_POSIX) ? 0 : 1;
						c = IORDWRSYM;
						fcgetc();
						if((n = fcgetc())==';')
						{
							lp->token = c = IORDWRSYMT;
							if(lp->inexec)
								sh_syntax(lp,0);
						}
						else if(n>0)
							fcseek(-LEN);
						n= 0;
					}
					else if(n=='#' && (c=='<'||c=='>'))
						c |= SYMSHARP;
					else if(n==';' && c=='>')
					{
						c |= SYMSEMI;
						if(lp->inexec)
						{
							lp->token = c;
							sh_syntax(lp,0);
						}
					}
					else
						n = 0;
					if(n)
					{
						fcseek(1);
						lp->lex.incase = (c==BREAKCASESYM || c==FALLTHRUSYM);
					}
					else
					{
						if(lp->lexd.warn && (n=fcpeek(0))!=RPAREN && n!=' ' && n!='\t')
							errormsg(SH_DICT,ERROR_warn(0),e_lexspace,sh.inlineno,c,n);
					}
				}
				if(c==LPAREN && lp->comp_assign && !lp->lex.intest && !lp->lex.incase)
					lp->comp_assign = 2;
				else
					lp->comp_assign = 0;
				return lp->token=c;
			case S_ESC:
				/* check for \<new-line> */
				n = fcgetc();
				c=2;
#if SHOPT_CRNL
				if(n=='\r')
				{
					if((n = fcgetc())=='\n')
						c=3;
					else
					{
						n='\r';
						fcseek(-LEN);
					}
				}
#endif /* SHOPT_CRNL */
				if(n=='\n')
				{
					Sfio_t *sp;
					struct argnod *ap;
					sh.inlineno++;
					/* synchronize */
					if(!(sp=fcfile()))
						state=fcseek(0);
					fcclose();
					ap = lp->arg;
					if(sp)
						fcfopen(sp);
					else
						fcsopen((char*)state);
					/* remove \new-line */
					n = stktell(sh.stk)-c;
					stkseek(sh.stk,n);
					lp->arg = ap;
					if(n<=ARGVAL)
					{
						mode = 0;
						lp->lexd.first = 0;
					}
					continue;
				}
				wordflags |= ARG_QUOTED;
				if(mode==ST_DOL)
					goto err;
#ifndef STR_MAXIMAL
				else if(mode==ST_NESTED && lp->lexd.warn &&
					endchar(lp)==RBRACE &&
					sh_lexstates[ST_DOL][n]==S_DIG
				)
					errormsg(SH_DICT,ERROR_warn(0),e_lexfuture,sh.inlineno,n);
#endif /* STR_MAXIMAL */
				break;
			case S_NAME:
				if(!lp->lex.skipword)
					lp->lex.reservok *= 2;
				/* FALLTHROUGH */
			case S_TILDE:
				if(c=='~' && mode==ST_NESTED)
				{
					if(endchar(lp)==RBRACE)
					{
						lp->lexd.nested_tilde++;
						goto tilde;
					}
					continue;
				}
				/* FALLTHROUGH */
			case S_RES:
				varnamefirst = fcseek(0) - LEN;
				if(!lp->lexd.dolparen)
					lp->lexd.first = fcseek(0)-LEN;
				else if(lp->lexd.docword)
					lp->lexd.docend = fcseek(0)-LEN;
				mode = ST_NAME;
				if(c=='.')
					fcseek(-LEN);
				if(n!=S_TILDE)
					continue;
			tilde:
				n = fcgetc();
				if(n>0)
				{
					if(c=='~' && n==LPAREN)
					{
						if(lp->lexd.nested_tilde)
							lp->lexd.nested_tilde++;
						else if(lp->lex.incase)
							lp->lex.incase = TEST_RE;
					}
					fcseek(-LEN);
					if(lp->lexd.nested_tilde)
					{
						lp->lexd.nested_tilde--;
						continue;
					}
				}
				if(n==LPAREN)
					goto epat;
				wordflags = ARG_MAC;
				mode = ST_NORM;
				continue;
			case S_REG:
				if(mode==ST_BEGIN)
				{
				do_reg:
					/* skip new-line joining if called from comsub() */
					if(c=='\\' && fcpeek(0)=='\n' && !lp->lexd.dolparen)
					{
						sh.inlineno++;
						fcseek(1);
						continue;
					}
					fcseek(-LEN);
					if(!lp->lexd.dolparen)
						lp->lexd.first = fcseek(0);
					else if(lp->lexd.docword)
						lp->lexd.docend = fcseek(0);
					if(c=='[' && lp->assignok>=SH_ASSIGN)
					{
						mode = ST_NAME;
						continue;
					}
				}
				mode = ST_NORM;
				continue;
			case S_LIT:
				if(oldmode(lp)==ST_NONE && !lp->lexd.inlexskip)	/* in ((...)) */
				{
					if((c=fcpeek(0))==LPAREN || c==RPAREN || c=='$' || c==LBRACE || c==RBRACE || c=='[' || c==']')
					{
						if(fcpeek(1)=='\'')
							fcseek(2);
					}
					continue;
				}
				wordflags |= ARG_QUOTED;
				if(mode==ST_DOL)
				{
					if(endchar(lp)!='$')
						goto err;
					if(oldmode(lp)==ST_QUOTE) /* $' within "" or `` */
					{
						if(lp->lexd.warn)
							errormsg(SH_DICT,ERROR_warn(0),e_lexslash,sh.inlineno);
						mode = ST_LIT;
					}
				}
				if(mode!=ST_LIT)
				{
					if(lp->lexd.warn && lp->lex.last_quote && sh.inlineno > lp->lastline && fcpeek(-2)!='$')
						errormsg(SH_DICT,ERROR_warn(0),e_lexlongquote,lp->lastline,lp->lex.last_quote);
					lp->lex.last_quote = 0;
					lp->lastline = sh.inlineno;
					if(mode!=ST_DOL)
						pushlevel(lp,'\'',mode);
					mode = ST_LIT;
					continue;
				}
				/* check for multi-line single-quoted string */
				else if(sh.inlineno > lp->lastline)
					lp->lex.last_quote = '\'';
				mode = oldmode(lp);
				poplevel(lp);
				break;
			case S_ESC2:
				/* \ inside '' */
				if(endchar(lp)=='$')
				{
					n = fcgetc();
					if(n=='\n')
						sh.inlineno++;
				}
				continue;
			case S_GRAVE:
				if(lp->lexd.warn && (mode!=ST_QUOTE || endchar(lp)!='`'))
					errormsg(SH_DICT,ERROR_warn(0),e_lexobsolete1,sh.inlineno);
				wordflags |=(ARG_MAC|ARG_EXP);
				if(mode==ST_QUOTE)
					ingrave = !ingrave;
				/* FALLTHROUGH */
			case S_QUOTE:
				if(oldmode(lp)==ST_NONE && lp->lexd.arith)	/* in ((...)) */
				{
					if(n!=S_GRAVE || fcpeek(0)=='\'')
						continue;
				}
				if(n==S_QUOTE)
					wordflags |=ARG_QUOTED;
				if(mode!=ST_QUOTE)
				{
					if(c!='"' || mode!=ST_QNEST)
					{
						if(lp->lexd.warn && lp->lex.last_quote && sh.inlineno > lp->lastline)
							errormsg(SH_DICT,ERROR_warn(0),e_lexlongquote,lp->lastline,lp->lex.last_quote);
						lp->lex.last_quote=0;
						lp->lastline = sh.inlineno;
						pushlevel(lp,c,mode);
					}
					ingrave ^= (c=='`');
					mode = ST_QUOTE;
					continue;
				}
				else if((n=endchar(lp))==c)
				{
					if(sh.inlineno > lp->lastline)
						lp->lex.last_quote = c;
					/*
					 * At this point, we know that the previous skipping of characters was done
					 * according to the ST_QUOTE state table. We also know that the character that
					 * stopped the skipping is also the ending character for the current level. If
					 * that character was " and if we are in a `...` statement, then don't switch
					 * to the old mode, as we are actually at the innermost section of a "`"..."`"
					 * statement, which should be skipped again using the ST_QUOTE state table.
					 */
					if(c!='"' || !ingrave)
					{
						mode = oldmode(lp);
						poplevel(lp);
					}
				}
				else if(c=='"' && n==RBRACE)
					mode = ST_QNEST;
				break;
			case S_DOL:
				/* don't check syntax inside `` */
				if(mode==ST_QUOTE && ingrave)
					continue;
#if SHOPT_KIA
				if(lp->lexd.first)
					kia.offset = fcseek(0)-lp->lexd.first;
				else
					kia.offset = stktell(sh.stk)+fcseek(0)-fcfirst();
#endif /* SHOPT_KIA */
				pushlevel(lp,'$',mode);
				mode = ST_DOL;
				continue;
			case S_PAR:
			do_comsub:
				wordflags |= ARG_MAC;
				mode = oldmode(lp);
				poplevel(lp);
				fcseek(-LEN);
				n = lp->digits;
				wordflags |= comsub(lp,c);
				lp->digits = n;
				continue;
			case S_RBRA:
				if((n=endchar(lp)) == '$')
					goto err;
				if(mode!=ST_QUOTE || n==RBRACE)
				{
					mode = oldmode(lp);
					poplevel(lp);
				}
				break;
			case S_EDOL:
				/* end $identifier */
#if SHOPT_KIA
				if(kia.file)
					refvar(lp,0);
#endif /* SHOPT_KIA */
				if(lp->lexd.warn && c==LBRACT && !lp->lex.intest && !lp->lexd.arith && oldmode(lp)!= ST_NESTED)
					errormsg(SH_DICT,ERROR_warn(0),e_lexusebrace,sh.inlineno);
				fcseek(-LEN);
				mode = oldmode(lp);
				poplevel(lp);
				break;
			case S_DOT:
				if(varnamelength && fcpeek(-LEN - 1)==']')
					varnamelength = 0;
				/* make sure next character is alpha */
				if((n = fcgetc()) > 0)
				{
					if(n=='.')
						n = fcgetc();
					if(n>0)
						fcseek(-LEN);
				}
				if(isaletter(n) || n==LBRACT)
					continue;
				if(mode==ST_NAME)
				{
					if(n=='=')
						continue;
					break;
				}
				else if(n==RBRACE)
					continue;
				if(isastchar(n))
					continue;
				goto err;
			case S_SPC1:
				wordflags |= ARG_MAC;
				if(endchar(lp)==RBRACE)
				{
					setchar(lp,c);
					continue;
				}
				/* FALLTHROUGH */
			case S_ALP:
				if(c=='.' && endchar(lp)=='$')
					goto err;
				/* FALLTHROUGH */
			case S_SPC2:
			case S_DIG:
				wordflags |= ARG_MAC;
				switch(endchar(lp))
				{
					case '$':
						if(n==S_ALP) /* $identifier */
							mode = ST_DOLNAME;
						else
						{
							mode = oldmode(lp);
							poplevel(lp);
						}
						break;
					case '@':
					case '!':
						if(n!=S_ALP)
							goto dolerr;
						/* FALLTHROUGH */
					case '#':
						if(c=='#')
							n = S_ALP;
						/* FALLTHROUGH */
					case RBRACE:
						if(n==S_ALP)
						{
							setchar(lp,RBRACE);
							if(c=='.')
								fcseek(-LEN);
							mode = ST_BRACE;
						}
						else
						{
							if((c = fcgetc()) > 0)
								fcseek(-LEN);
							if(state[c]==S_ALP)
								goto err;
							if(n==S_DIG)
								setchar(lp,'0');
							else
								setchar(lp,'!');
						}
						break;
					case '0':
						if(n==S_DIG)
							break;
					default:
						goto dolerr;
				}
				break;
			dolerr:
			case S_ERR:
				if((n=endchar(lp)) == '$')
					goto err;
				if(c=='*' || (n=sh_lexstates[ST_BRACE][c])!=S_MOD1 && n!=S_MOD2)
				{
					/* see whether inside `...` */
					if((n = endchar(lp)) != '`')
						goto err;
					mode = oldmode(lp);
					poplevel(lp);
					pushlevel(lp,RBRACE,mode);
				}
				else
					setchar(lp,RBRACE);
				mode = ST_NESTED;
				continue;
			case S_MOD1:
				mode = ST_MOD1;
				continue;
			case S_MOD2:
#if SHOPT_KIA
				if(kia.file)
					refvar(lp,1);
#endif /* SHOPT_KIA */
				if(c!=':' && (n = fcgetc()) > 0)
				{
					if(n!=c)
						c = 0;
					if(!c || (n = fcgetc()) > 0)
					{
						fcseek(-LEN);
						if(n==LPAREN)
						{
							if(c!='%')
							{
								lp->token = n;
								sh_syntax(lp,0);
							}
							else if(lp->lexd.warn)
								errormsg(SH_DICT,ERROR_warn(0),e_lexquote,sh.inlineno,'%');
						}
					}
				}
				lp->lex.nestedbrace = 0;
				mode = ST_NESTED;
				continue;
			case S_LBRA:
				if((c=endchar(lp)) == '$')
				{
					if((c = fcgetc()) > 0)
						fcseek(-LEN);
					setchar(lp,RBRACE);
					if(state[c]!=S_ERR && c!=RBRACE)
						continue;
					if((n=sh_lexstates[ST_BEGIN][c])==0 || n==S_OP || n==S_NLTOK)
					{
						c = LBRACE;
						goto do_comsub;
					}
				}
			err:
				if(iswalpha(c))
					continue;
				n = endchar(lp);
				mode = oldmode(lp);
				poplevel(lp);
				if(n!='$')
				{
					lp->token = c;
					sh_syntax(lp,0);
				}
				else
				{
					if(lp->lexd.warn && c!='/' && sh_lexstates[ST_NORM][c]!=S_BREAK && (c!='"' || mode==ST_QUOTE))
						errormsg(SH_DICT,ERROR_warn(0),e_lexslash,sh.inlineno);
					else if(c=='"' && mode!=ST_QUOTE && !ingrave)
						wordflags |= ARG_MESSAGE;
					fcseek(-LEN);
				}
				continue;
			case S_META:
				if(lp->lexd.warn && endchar(lp)==RBRACE && !lp->lexd.nested_tilde)
					errormsg(SH_DICT,ERROR_warn(0),e_lexusequote,sh.inlineno,c);
				continue;
			case S_PUSH:
				n = fcgetc();
				if(n==RPAREN)
					continue;
				else
					fcseek(-LEN);
				pushlevel(lp,RPAREN,mode);
				mode = ST_NESTED;
				continue;
			case S_POP:
			do_pop:
				if(c==RBRACE && mode==ST_NESTED && lp->lex.nestedbrace)
				{
					lp->lex.nestedbrace--;
					continue;
				}
				if(lp->lexd.level <= inlevel)
					break;
				if(lp->lexd.level==inlevel+1 && lp->lex.incase>=TEST_RE && !lp->lex.intest && c!=RBRACE)
				{
					fcseek(-LEN);
					goto breakloop;
				}
				n = endchar(lp);
				if(c==RBRACT  && !(n==RBRACT || n==RPAREN))
					continue;
				if((c==RBRACE||c==RPAREN) && n==RPAREN)
				{
					if((n = fcgetc())==LPAREN)
					{
						if(c!=RPAREN)
							fcseek(-LEN);
						continue;
					}
					if(n>0)
						fcseek(-LEN);
					n = RPAREN;
				}
				if(c==RBRACE)
					lp->lexd.nested_tilde = 0;
				if(c==';' && n!=';')
				{
					if(lp->lexd.warn && n==RBRACE)
						errormsg(SH_DICT,ERROR_warn(0),e_lexusequote,sh.inlineno,c);
					continue;
				}
				if(mode==ST_QNEST)
				{
					if(lp->lexd.warn)
						errormsg(SH_DICT,ERROR_warn(0),e_lexescape,sh.inlineno,c);
					continue;
				}
				mode = oldmode(lp);
				poplevel(lp);
				if(epatchar!='~')
					epatchar = '@';
				/* quotes in subscript need expansion */
				if(mode==ST_NAME && (wordflags&ARG_QUOTED))
					wordflags |= ARG_MAC;
				/* check for ((...)) */
				if(n==1 && c==RPAREN)
				{
					if((n = fcgetc())==RPAREN)
					{
						if(mode==ST_NONE && !lp->lexd.dolparen)
							goto breakloop;
						lp->lex.reservok = 1;
						lp->lex.skipword = 0;
						return lp->token=EXPRSYM;
					}
					/* backward compatibility */
					{
						if(lp->lexd.warn)
							errormsg(SH_DICT,ERROR_warn(0),e_lexnested,sh.inlineno);
						if(!(state=lp->lexd.first))
							state = fcfirst();
						else
						{
							n = state-fcseek(0);
							fcseek(n);
						}
						lp->lexd.paren = 1;
					}
					return lp->token=LPAREN;
				}
				if(mode==ST_NONE)
					return 0;
				if(c!=n && lp->lex.incase<TEST_RE)
				{
					lp->token = c;
					sh_syntax(lp,0);
				}
				if(c==RBRACE && (mode==ST_NAME||mode==ST_NORM))
					goto epat;
				continue;
			case S_EQ:
				if(varnamefirst && !varnamelength)
				{
					varnamelength = fcseek(0) - LEN - varnamefirst;
					if(varnamelength > 0 && fcpeek(-LEN - 1) == '+')
						varnamelength--;  /* += */
				}
				assignment = lp->assignok;
				/* FALLTHROUGH */
			case S_COLON:
				if(assignment)
				{
					if((c = fcgetc())=='~')
						wordflags |= ARG_MAC;
					else if(c!=LPAREN && assignment==SH_COMPASSIGN)
						assignment = 0;
					if(c!=EOF)
						fcseek(-LEN);
				}
				break;
			case S_BRACT:
				if(varnamefirst && !varnamelength && fcpeek(-LEN - 1)!='.')
					varnamelength = fcseek(0) - LEN - varnamefirst;
				/* check for possible subscript */
				if((n=endchar(lp))==RBRACT || n==RPAREN ||
					(mode==ST_BRACE) ||
					(oldmode(lp)==ST_NONE) ||
					(mode==ST_NAME && (lp->assignok||lp->lexd.level)))
				{
					n = fcgetc();
					if(n>0 && n==']')
					{
						if(mode==ST_NAME)
							sh_syntax(lp,1);
						if(!epatchar || epatchar=='%')
							continue;
					}
					else
						fcseek(-LEN);
					pushlevel(lp,RBRACT,mode);
					wordflags |= ARG_QUOTED;
					mode = ST_NESTED;
					continue;
				}
				wordflags |= ARG_EXP;
				break;
			case S_BRACE:
			{
				int isfirst;
				if(lp->lexd.dolparen)
				{
					if(mode==ST_BEGIN && (lp->lex.reservok||lp->comsub))
					{
						if(lp->comsub)
							return lp->token=c;
						n = fcgetc();
						if(n>0)
							fcseek(-LEN);
						else
							n = '\n';
						if(n==RBRACT || sh_lexstates[ST_NORM][n])
							return lp->token=c;
					}
					break;
				}
				else if(mode==ST_NESTED && endchar(lp)==RBRACE)
				{
					lp->lex.nestedbrace++;
					continue;
				}
				else if(mode==ST_BEGIN)
				{
					if(lp->comsub && c==RBRACE)
						return lp->token=c;
					goto do_reg;
				}
				isfirst = (lp->lexd.first&&fcseek(0)==lp->lexd.first+1);
				if((n = fcgetc()) <= 0)
					break;
				/* check for {} */
				if(c==LBRACE && n==RBRACE)
					break;
				fcseek(-LEN);
				/* check for reserved word { or } */
				if(lp->lex.reservok && state[n]==S_BREAK && isfirst)
					break;
#if SHOPT_BRACEPAT
				if(sh_isoption(SH_BRACEEXPAND) && c==LBRACE && !assignment && state[n]!=S_BREAK
					&& !lp->lex.incase && !lp->lex.intest
					&& !lp->lex.skipword)
				{
					wordflags |= ARG_EXP;
				}
#endif
				if(c==RBRACE && n==LPAREN)
					goto epat;
				break;
			}
			case S_PAT:
				wordflags |= ARG_EXP;
				/* FALLTHROUGH */
			case S_EPAT:
			epat:
				if((n = fcgetc())==LPAREN && c!='[')
				{
					epatchar = c;
					if(lp->lex.incase==TEST_RE)
					{
						lp->lex.incase++;
						pushlevel(lp,RPAREN,ST_NORM);
						mode = ST_NESTED;
					}
					wordflags |= ARG_EXP;
					pushlevel(lp,RPAREN,mode);
					mode = ST_NESTED;
					continue;
				}
				if(lp->lexd.warn && c=='[' && n=='^')
					errormsg(SH_DICT,ERROR_warn(0),e_lexcharclass,sh.inlineno);
				if(n>0)
					fcseek(-LEN);
				if(n=='=' && c=='+' && mode==ST_NAME)
					continue;
				break;
		}
		lp->comp_assign = 0;
		if(mode==ST_NAME)
			mode = ST_NORM;
		else if(mode==ST_NONE)
			return 0;
	}
breakloop:
	if(lp->lexd.nocopy)
		return 0;
	if(lp->lexd.dolparen)
	{
		if(lp->lexd.docword)
			nested_here(lp);
		lp->lexd.message = (wordflags&ARG_MESSAGE);
		return lp->token=0;
	}
	if(!(state=lp->lexd.first))
		state = fcfirst();
	n = fcseek(0)-(char*)state;
	if(!lp->arg)
		lp->arg = stkseek(sh.stk,ARGVAL);
	if(n>0)
		sfwrite(sh.stk,state,n);
	sfputc(sh.stk,0);
	stkseek(sh.stk,stktell(sh.stk)-1);
	state = stkptr(sh.stk,ARGVAL);
	n = stktell(sh.stk)-ARGVAL;
	lp->lexd.first=0;
	if(n==1)
	{
		/* check for numbered redirection */
		n = state[0];
		if(!lp->lex.intest && (c=='<' || c=='>') && isadigit(n))
		{
			c = sh_lex(lp);
			lp->digits = (n-'0');
			return c;
		}
		if(n==LBRACT)
			c = 0;
		else if(n==RBRACE && lp->comsub)
			return lp->token=n;
		else if(n=='~')
			c = ARG_MAC;
		else
			c = (wordflags&ARG_EXP);
		n = 1;
	}
	else if(n>2 && state[0]=='{' && state[n-1]=='}' && !lp->lex.intest && !lp->lex.incase && (c=='<' || c== '>'))
	{
		if(!strchr(state,','))
		{
			/* Redirection of the form {varname}>file, etc. */
			stkseek(sh.stk,stktell(sh.stk)-1);
			lp->arg = stkfreeze(sh.stk,1);
			return lp->token=IOVNAME;
		}
		c = wordflags;
	}
	else
		c = wordflags;
	if(assignment || (lp->lex.intest&&!lp->lex.incase) || mode==ST_NONE)
		c &= ~ARG_EXP;
	if((c&ARG_EXP) && (c&ARG_QUOTED))
		c |= ARG_MAC;
	if(mode==ST_NONE)
	{
		/* eliminate trailing )) */
		stkseek(sh.stk,stktell(sh.stk)-2);
	}
	if(c&ARG_MESSAGE)
	{
		if(sh_isoption(SH_DICTIONARY))
			lp->arg = endword(2);
		c |= ARG_MAC;
	}
	if(c==0 || (c&(ARG_MAC|ARG_EXP|ARG_MESSAGE)))
	{
		lp->arg = stkfreeze(sh.stk,1);
		lp->arg->argflag = (c?c:ARG_RAW);
	}
	else if(mode==ST_NONE)
		lp->arg = endword(-1);
	else
		lp->arg = endword(0);
	state = lp->arg->argval;
	lp->comp_assign = assignment;
	if(assignment)
	{
		lp->arg->argflag |= ARG_ASSIGN;
		lp->varnamelength = varnamelength;
		if(sh_isoption(SH_NOEXEC))
		{
			char *cp = strchr(state, '=');
			if(cp && strncmp(++cp, "$((", 3) == 0)
				errormsg(SH_DICT, ERROR_warn(0), e_lexarithwarn, sh.inlineno,
					state, cp - state, state, cp + 3);
		}
	}
	else if(!lp->lex.skipword)
		lp->assignok = 0;
	lp->arg->argchn.cp = 0;
	lp->arg->argnxt.ap = 0;
	if(mode==ST_NONE)
		return lp->token=EXPRSYM;
	if(lp->lex.intest)
	{
		if(lp->lex.testop1)
		{
			lp->lex.testop1 = 0;
			if(n==2 && state[0]=='-' && state[2]==0 &&
				strchr(test_opchars,state[1]))
			{
				if(lp->lexd.warn && state[1]=='a')
					errormsg(SH_DICT,ERROR_warn(0),e_lexobsolete2,sh.inlineno);
				lp->digits = state[1];
				lp->token = TESTUNOP;
			}
			else if(n==1 && state[0]=='!' && state[1]==0)
			{
				lp->lex.testop1 = 1;
				lp->token = '!';
			}
			else
			{
				lp->lex.testop2 = 1;
				lp->token = 0;
			}
			return lp->token;
		}
		lp->lex.incase = 0;
		if(state[0]==']' && state[1]==']' && !state[2])
		{
			/* end of [[ ... ]] */
			lp->lex.testop2 = lp->lex.intest = 0;
			lp->lex.reservok = 1;
			lp->token = ETESTSYM;
			return lp->token;
		}
		c = sh_lookup(state,shtab_testops);
		switch(c)
		{
		case TEST_SEQ:
			if(lp->lexd.warn && state[1]==0)
				errormsg(SH_DICT,ERROR_warn(0),e_lexobsolete3,sh.inlineno);
			/* FALLTHROUGH */
		default:
			if(lp->lex.testop2)
			{
				if(lp->lexd.warn && (c&TEST_ARITH))
				{
					char *alt;
					switch(c)
					{
					    case TEST_EQ:
						alt = "==";  /* '-eq' --> '==' */
						break;
					    case TEST_NE:
						alt = "!=";  /* '-ne' --> '!=' */
						break;
					    case TEST_LT:
						alt = "<";   /* '-lt' --> '<' */
						break;
					    case TEST_GT:
						alt = ">";   /* '-gt' --> '>' */
						break;
					    case TEST_LE:
						alt = "<=";  /* '-le' --> '<=' */
						break;
					    case TEST_GE:
						alt = ">=";  /* '-ge' --> '>=' */
						break;
					    default:
#if _AST_release
						alt = NULL;	/* output '(null)' (should never happen) */
#else
						abort();
#endif
						break;
					}
					errormsg(SH_DICT, ERROR_warn(0), e_lexobsolete4,
							sh.inlineno, state, alt);
				}
				if(c&TEST_STRCMP)
					lp->lex.incase = 1;
				else if(c==TEST_REP)
					lp->lex.incase = TEST_RE;
				lp->lex.testop2 = 0;
				lp->digits = c;
				lp->token = TESTBINOP;
				return lp->token;
			}
			/* FALLTHROUGH */
		case TEST_OR: case TEST_AND:
		case 0:
			return lp->token=0;
		}
	}
	if(lp->lex.reservok /* && !lp->lex.incase*/ && n<=2)
	{
		/* check for {, }, ! */
		c = state[0];
		if(n==1 && (c=='{' || c=='}' || c=='!'))
		{
			if(lp->lexd.warn && c=='{' && lp->lex.incase==2)
				errormsg(SH_DICT,ERROR_warn(0),e_lexobsolete6,sh.inlineno);
			if(lp->lex.incase==1 && c==RBRACE)
				lp->lex.incase = 0;
			return lp->token=c;
		}
		else if(!lp->lex.incase && c==LBRACT && state[1]==LBRACT)
		{
			lp->lex.intest = lp->lex.testop1 = 1;
			lp->lex.testop2 = lp->lex.reservok = 0;
			return lp->token=BTESTSYM;
		}
	}
	c = 0;
	if(!lp->lex.skipword)
	{
		if(n>1 && lp->lex.reservok==1 && mode==ST_NAME &&
			(c=sh_lookup(state,shtab_reserved)))
		{
			if(lp->lex.incase)
			{
				if(lp->lex.incase >1)
					lp->lex.incase = 1;
				else if(c==ESACSYM)
					lp->lex.incase = 0;
				else
					c = 0;
			}
			else if(c==FORSYM || c==CASESYM || c==SELECTSYM || c==FUNCTSYM || c==NSPACESYM)
			{
				lp->lex.skipword = 1;
				lp->lex.incase = 2*(c==CASESYM);
			}
			else
				lp->lex.skipword = 0;
			if(c==INSYM && !lp->lex.incase)
				lp->lex.reservok = 0;
			else if(c==TIMESYM)
			{
				/* POSIX requires time -p */
				while((n = fcgetc())==' ' || n=='\t');
				if(n>0)
					fcseek(-LEN);
				if(n=='-')
					c=0;
			}
			return lp->token=c;
		}
		if(!(wordflags&ARG_QUOTED) && (lp->lex.reservok||lp->aliasok))
		{
			/* check for aliases */
			Namval_t* np;
			if(!lp->lex.incase && !assignment && fcpeek(0)!=LPAREN &&
				(np=nv_search(state,sh.alias_tree,0))
				&& !nv_isattr(np,NV_NOEXPAND)
				&& (lp->aliasok!=2 || nv_isattr(np,BLT_DCL))
				&& (!sh_isstate(SH_NOALIAS) || nv_isattr(np,NV_NOFREE))
				&& (state=nv_getval(np)))
			{
				setupalias(lp,state,np);
				nv_onattr(np,NV_NOEXPAND);
				lp->lex.reservok = 1;
				lp->assignok |= lp->lex.reservok;
				return sh_lex(lp);
			}
		}
		lp->lex.reservok = 0;
	}
	lp->lex.skipword = lp->lexd.docword = 0;
	return lp->token=c;
}

/*
 * read to end of command substitution
 * of the form $(...) or ${ ...;}
 * or arithmetic expansion $((...))
 *
 * Ugly hack alert: At parse time, command substitutions and arithmetic expansions are read
 * without parsing, using lexical analysis only. This is only to determine their length, so
 * that their literal source text can be stored in the parse tree. They are then actually
 * parsed at runtime (!) each time they are executed (!) via comsubst() in macro.c.
 *
 * This approach is okay for arithmetic expansions, but for command substitutions it is an
 * unreliable hack. The lexer does not have real shell grammar knowledge; that's what the
 * parser is for. However, a clean separation between lexical analysis and parsing is not
 * possible, because the design of the shell language is fundamentally messy. So we need the
 * parser to set the some flags in the lexer at the appropriate times to avoid spurious
 * syntax errors (these are the non-private Lex_t struct members). But the parser obviously
 * cannot do this if we're not using it.
 *
 * The comsub() hack below, along with all the dolparen checks in the lexer, tries to work
 * around this fundamental problem as best we can to make it work in all but corner cases.
 * It sets the lexd.dolparen, lexd.dolparen_eqparen and lexd.dolparen_arithexp flags for the
 * rest of the lexer code to execute lots of workarounds.
 *
 * TODO: to achieve correctness, actually parse command substitutions at parse time.
 */
static int comsub(Lex_t *lp, int endtok)
{
	int n,c;
	unsigned short count=1;
	int line=sh.inlineno;
	struct ionod *inheredoc = lp->heredoc;
	char save_arithexp = lp->lexd.dolparen_arithexp;
	char *first,*cp=fcseek(0),word[5];
	int off, messages=0, assignok=lp->assignok, csub;
	struct _shlex_pvt_lexstate_ save = lp->lex;
	csub = lp->comsub;
	sh_lexopen(lp,1);
	lp->lexd.dolparen++;
	lp->lexd.dolparen_arithexp = endtok==LPAREN && fcpeek(1)==LPAREN;  /* $(( */
	lp->lex.incase=0;
	pushlevel(lp,0,0);
	lp->comsub = (endtok==LBRACE);
	if(first=lp->lexd.first)
		off = cp-first;
	else
		off = cp-fcfirst();
	if(off<0)
		c=*cp, *cp=0;
	n = sh_lex(lp);
	if(off<0)
		*cp = c;
	if(n==endtok || off<0)
	{
		if(endtok==LPAREN && lp->lexd.paren)
		{
			if(first==lp->lexd.first)
			{
				n = cp+1-(char*)fcseek(0);
				fcseek(n);
			}
			count++;
			lp->lexd.paren = 0;
			fcgetc();
		}
		while(1)
		{
			/* look for case and esac */
			n=0;
			while(1)
			{
				c = fcgetc();
				/* skip leading white space */
				if(n==0 && !sh_lexstates[ST_BEGIN][c])
					continue;
				if(n==4)
					break;
				if(sh_lexstates[ST_NAME][c])
				{
					if(c==' ' || c=='\t')
					{
						n = 0;
						continue;
					}
					goto skip;
				}
				word[n++] = c;
			}
			if(sh_lexstates[ST_NAME][c]==S_BREAK)
			{
				if(strncmp(word,"case",4)==0)
					lp->lex.incase=1;
				else if(strncmp(word,"esac",4)==0)
					lp->lex.incase=0;
			}
		skip:
			if(c && (c!='#' || n==0))
				fcseek(-LEN);
			if(c==RBRACE && lp->lex.incase)
				lp->lex.incase=0;
			c=sh_lex(lp);
			switch(c)
			{
			    case LBRACE:
				if(endtok==LBRACE && !lp->lex.incase)
				{
					lp->comsub = 0;
					count++;
				}
				break;
			    case RBRACE:
			    rbrace:
				if(endtok==LBRACE && --count<=0)
					goto done;
				if(count==1)
					lp->comsub = endtok==LBRACE;
				break;
			    case IPROCSYM:	case OPROCSYM:
			    case LPAREN:
				/* lexd.dolparen_eqparen flags up "=(": we presume it's a compound assignment.
				 * This is a workaround for <https://github.com/ksh93/ksh/issues/269>. */
				if(!lp->lexd.dolparen_eqparen && fcpeek(-2)=='=')
					lp->lexd.dolparen_eqparen = count;
				if(endtok==LPAREN && !lp->lex.incase)
					count++;
				break;
			    case RPAREN:
				if(lp->lexd.dolparen_eqparen >= count)
					lp->lexd.dolparen_eqparen = 0;
				if(lp->lex.incase)
					lp->lex.incase=0;
				else if(endtok==LPAREN && --count<=0)
					goto done;
				break;
			    case EOFSYM:
				lp->lastline = line;
				lp->lasttok = endtok;
				sh_syntax(lp,0);
				/* UNREACHABLE */
			    case IOSEEKSYM:
				if((c = fcgetc())!='#' && c>0)
					fcseek(-LEN);
				break;
			    case IODOCSYM:
				lp->lexd.docextra = 0;
				sh_lex(lp);
				break;
			    case 0:
				lp->lex.reservok = 0;
				messages |= lp->lexd.message;
				break;
			    case ';':
				do
					c = fcgetc();
				while(!sh_lexstates[ST_BEGIN][c]);
				if(c==RBRACE && endtok==LBRACE)
					goto rbrace;
				if(c>0)
					fcseek(-LEN);
				/* FALLTHROUGH */
			    default:
				lp->lex.reservok = 1;
			}
		}
	}
done:
	poplevel(lp);
	lp->comsub = csub;
	lp->lastline = line;
	lp->lexd.dolparen--;
	lp->lexd.dolparen_arithexp = save_arithexp;
	lp->lex = save;
	lp->assignok = (endchar(lp)==RBRACT?assignok:0);
	if(lp->heredoc && !inheredoc)
		/* here-document isn't fully contained in command substitution */
		sh_syntax(lp,3);
	return messages;
}

/*
 * here-doc nested in $(...)
 * allocate ionode with delimiter filled in without disturbing the stack
 */
static void nested_here(Lex_t *lp)
{
	struct ionod	*iop;
	int		n=0,offset;
	struct argnod	*arg = lp->arg;
	char		*base;
	if(offset=stktell(sh.stk))
		base = stkfreeze(sh.stk,0);
	if(lp->lexd.docend)
		n = fcseek(0)-lp->lexd.docend;
	iop = sh_newof(0,struct ionod,1,lp->lexd.docextra+n+ARGVAL);
	iop->iolst = lp->heredoc;
	stkseek(sh.stk,ARGVAL);
	if(lp->lexd.docextra)
	{
		sfseek(sh.strbuf,0, SEEK_SET);
		sfmove(sh.strbuf,sh.stk,lp->lexd.docextra,-1);
		sfseek(sh.strbuf,0, SEEK_SET);
	}
	sfwrite(sh.stk,lp->lexd.docend,n);
	lp->arg = endword(0);
	iop->ioname = (char*)(iop+1);
	strcpy(iop->ioname,lp->arg->argval);
	iop->iofile = (IODOC|IORAW);
	if(lp->lexd.docword>1)
		iop->iofile |= IOSTRIP;
	lp->heredoc = iop;
	lp->arg = arg;
	lp->lexd.docword = 0;
	if(offset)
		stkset(sh.stk,base,offset);
	else
		stkseek(sh.stk,0);
}

/*
 * skip to <close> character
 * if <copy> is non,zero, then the characters are copied to the stack
 * <state> is the initial lexical state
 */
void sh_lexskip(Lex_t *lp,int close, int copy, int  state)
{
	char	*cp;
	lp->lexd.nest = close;
	lp->lexd.lex_state = state;
	lp->lexd.inlexskip = 1;
	if(copy)
		fcnotify(lex_advance,lp);
	else
		lp->lexd.nocopy++;
	sh_lex(lp);
	lp->lexd.inlexskip = 0;
	if(copy)
	{
		fcnotify(0,lp);
		if(!(cp=lp->lexd.first))
			cp = fcfirst();
		if((copy = fcseek(0)-cp) > 0)
			sfwrite(sh.stk,cp,copy);
	}
	else
		lp->lexd.nocopy--;
}

#if SHOPT_CRNL
    ssize_t _sfwrite(Sfio_t *sp, const void *buff, size_t n)
    {
	const char *cp = (const char*)buff, *next=cp, *ep = cp + n;
	int m=0,k;
	while(next = (const char*)memchr(next,'\r',ep-next))
		if(*++next=='\n')
		{
			if(k=next-cp-1)
			{
				if((k=sfwrite(sp,cp,k)) < 0)
					return m>0?m:-1;
				m += k;
			}
			cp = next;
		}
	if((k=sfwrite(sp,cp,ep-cp)) < 0)
		return m>0?m:-1;
	return m+k;
    }
#   define sfwrite	_sfwrite
#endif /* SHOPT_CRNL */

/*
 * read in here-document from script
 * quoted here documents, and here-documents without special chars are
 * noted with the IOQUOTE flag
 * returns 1 for complete here-doc, 0 for EOF
 */
static int here_copy(Lex_t *lp,struct ionod *iop)
{
	const char	*state;
	int		c,n;
	char		*bufp,*cp;
	Sfio_t		*sp=sh.heredocs, *funlog;
	int		stripcol=0,stripflg, nsave, special=0;
	if(funlog=sh.funlog)
	{
		if(fcfill()>0)
			fcseek(-LEN);
		sh.funlog = 0;
	}
	if(iop->iolst)
		here_copy(lp,iop->iolst);
	iop->iooffset = sfseek(sp,0,SEEK_END);
	iop->iosize = 0;
	iop->iodelim=iop->ioname;
	/* check for and strip quoted characters in delimiter string */
	if(stripflg=iop->iofile&IOSTRIP)
	{
		while(*iop->iodelim=='\t')
			iop->iodelim++;
		/* skip over leading tabs in document */
		if(iop->iofile&IOLSEEK)
		{
			iop->iofile &= ~IOLSEEK;
			while((c = fcgetc())=='\t' || c==' ')
			{
				if(c==' ')
					stripcol++;
				else
					stripcol += 8 - stripcol%8;
			}
		}
		else
			while((c = fcgetc())=='\t');
		if(c>0)
			fcseek(-LEN);
	}
	if(iop->iofile&IOQUOTE)
		state = sh_lexstates[ST_LIT];
	else
		state = sh_lexstates[ST_QUOTE];
	bufp = fcseek(0);
	n = S_NL;
	while(1)
	{
		if(n!=S_NL)
		{
			/* skip over regular characters */
			do
			{
				if(mbsize(fcseek(0)) < 0 && fcleft() < MB_LEN_MAX)
				{
					n = S_EOF;
					SETLEN(-fcleft());
					break;
				}
			}
			while((n=STATE(state,c))==0);
		}
		if(n==S_EOF || !(c=fcget()))
		{
			if(LEN < 0)
				c = fclast()-bufp;
			else
				c= (fcseek(0)-1)-bufp;
			if(!lp->lexd.dolparen && c)
			{
				if(n==S_ESC)
					c--;
				if(!lp->lexd.dolparen && (c=sfwrite(sp,bufp,c))>0)
					iop->iosize += c;
			}
			if(LEN==0)
				SETLEN(1);
			if(LEN < 0)
			{
				n = LEN;
				c = fcmbget(&LEN);
				SETLEN(LEN + n);
			}
			else
				c = lexfill(lp);
			if(c<0)
				break;
			if(n==S_ESC)
			{
#if SHOPT_CRNL
				if(c=='\r' && (c=fcget())!=NL)
					fcseek(-LEN);
#endif /* SHOPT_CRNL */
				if(c==NL)
					fcseek(1);
				else if(!lp->lexd.dolparen)
				{
					iop->iosize++;
					sfputc(sp,'\\');
				}
			}
			if(LEN < 1)
				SETLEN(1);
			bufp = fcseek(-LEN);
		}
		else
			fcseek(-LEN);
		switch(n)
		{
		    case S_NL:
			sh.inlineno++;
			if((stripcol && c==' ') || (stripflg && c=='\t'))
			{
				if(!lp->lexd.dolparen)
				{
					/* write out line */
					n = fcseek(0)-bufp;
					if((n=sfwrite(sp,bufp,n))>0)
						iop->iosize += n;
				}
				/* skip over tabs */
				if(stripcol)
				{
					int col=0;
					do
					{
						c = fcgetc();
						if(c==' ')
							col++;
						else
							col += 8 - col%8;
						if(col>stripcol)
							break;
					}
					while (c==' ' || c=='\t');
				}
				else while(c=='\t')
					c = fcgetc();
				if(c<=0)
					goto done;
				bufp = fcseek(-LEN);
			}
			if(c!=iop->iodelim[0])
				break;
			cp = fcseek(0);
			nsave = n = 0;
			while(1)
			{
				if(!(c=fcget()))
				{
					if(!lp->lexd.dolparen && (c=cp-bufp))
					{
						if((c=sfwrite(sp,cp=bufp,c))>0)
							iop->iosize+=c;
					}
					nsave = n;
					if((c=lexfill(lp))<=0)
					{
						c = iop->iodelim[n]==0;
						goto done;
					}
				}
#if SHOPT_CRNL
				if(c=='\r' && (c=fcget())!=NL)
				{
					if(c)
						fcseek(-LEN);
					c='\r';
				}
#endif /* SHOPT_CRNL */
				if(c==NL)
					sh.inlineno++;
				if(iop->iodelim[n]==0 && (c==NL||c==RPAREN))
				{
					if(!lp->lexd.dolparen && (n=cp-bufp))
					{
						if((n=sfwrite(sp,bufp,n))>0)
							iop->iosize += n;
					}
					sh.inlineno--;
					if(c==RPAREN)
						fcseek(-LEN);
					goto done;
				}
				if(iop->iodelim[n++]!=c)
				{
					/*
					 * The match for delimiter failed.
					 * nsave>0 only when a buffer boundary
					 * was crossed while checking the
					 * delimiter
					 */
					if(!lp->lexd.dolparen && nsave>0)
					{
						if((n=sfwrite(sp,iop->iodelim,nsave))>0)
							iop->iosize += n;
						bufp = fcfirst();
					}
					if(c==NL)
						fcseek(-LEN);
					break;
				}
			}
			break;
		    case S_ESC:
			n=1;
#if SHOPT_CRNL
			if(c=='\r')
			{
				fcseek(1);
				if(c=fcget())
					fcseek(-LEN);
				if(c==NL)
					n=2;
				else
				{
					special++;
					break;
				}
			}
#endif /* SHOPT_CRNL */
			if(c==NL)
			{
				/* new-line joining */
				sh.inlineno++;
				if(!lp->lexd.dolparen && (n=(fcseek(0)-bufp)-n)>=0)
				{
					if(n && (n=sfwrite(sp,bufp,n))>0)
						iop->iosize += n;
					bufp = fcseek(0)+1;
				}
			}
			else
				special++;
			fcget();
			break;
		    case S_GRAVE:
		    case S_DOL:
			special++;
			break;
		}
		n=0;
	}
done:
	sh.funlog = funlog;
	if(lp->lexd.dolparen)
		free(iop);
	else if(!special)
		iop->iofile |= IOQUOTE;
	return c;
}

/*
 * generates string for given token
 */
static char	*fmttoken(Lex_t *lp, int sym)
{
	if(sym < 0)
		return (char*)sh_translate(e_lexzerobyte);
	if(sym==0)
		return lp->arg?lp->arg->argval:"?";
	if(lp->lex.intest && lp->arg && *lp->arg->argval)
		return lp->arg->argval;
	if(sym&SYMRES)
	{
		const Shtable_t *tp=shtab_reserved;
		while(tp->sh_number && tp->sh_number!=sym)
			tp++;
		return (char*)tp->sh_name;
	}
	if(sym==EOFSYM)
		return (char*)sh_translate(e_endoffile);
	if(sym==NL)
		return (char*)sh_translate(e_newline);
	stkfreeze(sh.stk,0);
	sfputc(sh.stk,sym);
	if(sym&SYMREP)
		sfputc(sh.stk,sym);
	else
	{
		switch(sym&SYMMASK)
		{
			case SYMAMP:
				sym = '&';
				break;
			case SYMPIPE:
				sym = '|';
				break;
			case SYMGT:
				sym = '>';
				break;
			case SYMLPAR:
				sym = LPAREN;
				break;
			case SYMSHARP:
				sym = '#';
				break;
			case SYMSEMI:
				if(*stkptr(sh.stk,0)=='<')
					sfputc(sh.stk,'>');
				sym = ';';
				break;
			default:
				sym = 0;
		}
		sfputc(sh.stk,sym);
	}
	return stkfreeze(sh.stk,1);
}

/*
 * print a bad syntax message
 */
noreturn void sh_syntax(Lex_t *lp, int special)
{
	const int eof = lp->token==EOFSYM && lp->lasttok;
	Sfio_t *sp;
	if((sp=fcfile()) || (sh.infd>=0 && (sp=sh.sftable[sh.infd])))
	{
		/* clear out any pending input */
		Sfio_t *top;
		while(fcget()>0);
		fcclose();
		while(top=sfstack(sp,SFIO_POPSTACK))
			sfclose(top);
	}
	else
		fcclose();
	sh.inlineno = lp->inlineno;
	sh.st.firstline = lp->firstline;
	/* construct error message */
	if (sh_isstate(SH_INTERACTIVE) || sh_isstate(SH_PROFILE))
		sfprintf(sh.strbuf, sh_translate(e_syntaxerror));
	else
		sfprintf(sh.strbuf, sh_translate(e_syntaxerror_at), eof ? sh.inlineno : lp->lastline);
	if (special==1)
		sfprintf(sh.strbuf, sh_translate(e_emptysubscr));
	else if (special==2)
		sfprintf(sh.strbuf, sh_translate(e_badreflist));
	else if (special==3)
		sfprintf(sh.strbuf, sh_translate(e_heredoccomsub), lp->heredoc->ioname);
	else if (eof)
		sfprintf(sh.strbuf, sh_translate(e_unmatched), fmttoken(lp, lp->lasttok));
	else
		sfprintf(sh.strbuf, sh_translate(e_unexpected), fmttoken(lp, lp->token));
	/* reset lexer state */
	sh_lexopen(lp, 0);
	errormsg(SH_DICT, ERROR_exit(SYNBAD), "%s", sfstruse(sh.strbuf));
	UNREACHABLE();
}

static unsigned char *stack_shift(unsigned char *sp, unsigned char *dp)
{
	unsigned char *ep;
	int offset = stktell(sh.stk);
	int left = offset - (sp - (unsigned char*)stkptr(sh.stk, 0));
	int shift = (dp+1-sp);
	offset += shift;
	stkseek(sh.stk,offset);
	sp = (unsigned char*)stkptr(sh.stk,offset);
	ep = sp - shift;
	while(left--)
		*--sp = *--ep;
	return sp;
}

/*
 * Assumes that current word is unfrozen on top of the stack
 * If <mode> is zero, gets rid of quoting and consider argument as string
 *    and returns pointer to frozen arg
 * If mode==1, just replace $"..." strings with international strings
 *    The result is left on the stack
 * If mode==2, the each $"" string is printed on standard output
 */
static struct argnod *endword(int mode)
{
	const char *const state = sh_lexstates[ST_NESTED];
	unsigned char *sp, *dp, *ep=0, *xp=0;	/* must be unsigned: pointed-to values used as index to 256-byte state table */
	int inquote=0, inlit=0;			/* set within quoted strings */
	int n, bracket=0;
	sfputc(sh.stk,0);
	sp =  (unsigned char*)stkptr(sh.stk,ARGVAL);
	if(mbwide())
	{
		do
		{
			int len;
			switch(len = mbsize(sp))
			{
			    case -1:	/* illegal multi-byte char */
			    case 0:
			    case 1:
				n=state[*sp++];
				break;
			    default:
				/*
				 * None of the state tables contain
				 * entries for multibyte characters,
				 * however, they should be treated
				 * the same as any other alpha
				 * character.  Therefore, we'll use
				 * the state of the 'a' character.
				 */
				n=state['a'];
				sp += len;
			}
		}
		while(n == 0);
	}
	else
		while((n = state[*sp++]) == 0)
			;
	dp = sp;
	if(mode<0)
		inquote = 1;
	while(1)
	{
		switch(n)
		{
		    case S_EOF:
		    {
			struct argnod* argp=0;
			stkseek(sh.stk,dp - (unsigned char*)stkptr(sh.stk,0));
			if(mode<=0)
			{
				argp = stkfreeze(sh.stk,0);
				argp->argflag = ARG_RAW|ARG_QUOTED;
			}
			return argp;
		    }
		    case S_LIT:
			if(!(inquote&1))
			{
				inlit = !inlit;
				if(mode==0 || (mode<0 && bracket))
				{
					dp--;
					if(ep)
					{
						*dp = 0;
						stresc((char*)ep);
						dp = ep + strlen((char*)ep);
					}
					ep = 0;
				}
			}
			break;
		    case S_QUOTE:
			if(mode<0 && !bracket)
				break;
			if(!inlit)
			{
				if(mode<=0)
					dp--;
				inquote = inquote^1;
				if(ep)
				{
					char *msg;
					if(mode==2)
					{
						sfprintf(sfstdout,"%.*s\n",dp-ep,ep);
						ep = 0;
						break;
					}
					*--dp = 0;
					msg = ERROR_translate(0,error_info.id,0,ep);
					n = strlen(msg);
					dp = ep+n;
					if(sp-dp <= 1)
					{
						sp = stack_shift(sp,dp);
						dp = sp-1;
						ep = dp-n;
					}
					memmove(ep,msg,n);
					*dp++ = '"';
				}
				ep = 0;
			}
			break;
		    case S_DOL:	/* check for $'...'  and $"..." */
			if(inlit)
				break;
			if(*sp==LPAREN || *sp==LBRACE)
			{
				inquote <<= 1;
				break;
			}
			if(inquote&1)
				break;
			if(*sp=='\'' || *sp=='"')
			{
				if(*sp=='"')
					inquote |= 1;
				else
					inlit = 1;
				sp++;
				if((mode==0||(mode<0&&bracket)) || (inquote&1))
				{
					if(mode==2)
						ep = dp++;
					else if(mode==1)
						(ep=dp)[-1] = '"';
					else
						ep = --dp;
				}
			}
			break;
		    case S_ESC:
#if SHOPT_CRNL
			if(*sp=='\r' && sp[1]=='\n')
				sp++;
#endif /* SHOPT_CRNL */
			if(inlit || mode>0)
			{
				if(mode<0)
				{
					if(dp>=sp)
					{
						sp = stack_shift(sp,dp+1);
						dp = sp-2;
					}
					*dp++ = '\\';
				}
				if(ep)
					*dp++ = *sp++;
				break;
			}
			n = *sp;
			if(!(inquote&1) || (sh_lexstates[ST_QUOTE][n] && n!=RBRACE))
			{
				if(n=='\n')
					dp--;
				else
					dp[-1] = n;
				sp++;
			}
			break;
		    case S_POP:
			if(sp[-1]!=RBRACT)
				break;
			if(!inlit && !(inquote&1))
			{
				inquote >>= 1;
				if(xp)
					dp = (unsigned char*)sh_checkid((char*)xp,(char*)dp);
				xp = 0;
				if(--bracket<=0 && mode<0)
					inquote = 1;
			}
			else if((inlit||inquote) && mode<0)
			{
				dp[-1] = '\\';
				if(dp>=sp)
				{
					sp = stack_shift(sp,dp);
					dp = sp-1;
				}
				*dp++ = ']';
			}
			break;
		    case S_BRACT:
			if(dp[-2]=='.')
				xp = dp;
			if(mode<0)
			{
				if(inlit || (bracket&&inquote))
				{
					dp[-1] = '\\';
					if(dp>=sp)
					{
						sp = stack_shift(sp,dp);
						dp = sp-1;
					}
					*dp++ = '[';
				}
				else if(bracket++==0)
					inquote = 0;
			}
			break;
		}
		if(mbwide())
		{
			do
			{
				int len;
				switch(len = mbsize(sp))
				{
				    case -1: /* illegal multi-byte char */
				    case 0:
				    case 1:
					n=state[*dp++ = *sp++];
					break;
				    default:
					/*
					 * None of the state tables contain
					 * entries for multibyte characters,
					 * however, they should be treated
					 * the same as any other alpha
					 * character.  Therefore, we'll use
					 * the state of the 'a' character.
					 */
					while(len--)
						*dp++ = *sp++;
					n=state['a'];
				}
			}
			while(n == 0);
		}
		else
			while((n=state[*dp++ = *sp++])==0)
				;
	}
}

struct alias
{
	Sfdisc_t	disc;
	Namval_t	*np;
	int		nextc;
	int		line;
	char		buf[2];
	Lex_t		*lp;
};

/*
 * This code gets called whenever an end of string is found with alias
 */
static int alias_exceptf(Sfio_t *iop,int type,void *data, Sfdisc_t *handle)
{
	struct alias *ap = (struct alias*)handle;
	Namval_t *np;
	Lex_t	*lp;
	NOT_USED(data);
	if(type==0 || type==SFIO_ATEXIT || !ap)
		return 0;
	lp = ap->lp;
	np = ap->np;
	if(type!=SFIO_READ)
	{
		if(type==SFIO_CLOSING)
		{
			Sfdisc_t *dp = sfdisc(iop,SFIO_POPDISC);
			if(dp!=handle)
				sfdisc(iop,dp);
		}
		else if(type==SFIO_DPOP || type==SFIO_FINAL)
			free(ap);
		goto done;
	}
	if(ap->nextc)
	{
		/* if last character is a blank, then next word can be an alias */
		int c = fcpeek(-1);
		if(isblank(c))
			lp->aliasok = 1;
		*ap->buf = ap->nextc;
		ap->nextc = 0;
		sfsetbuf(iop,ap->buf,1);
		return 1;
	}
done:
	if(np)
		nv_offattr(np,NV_NOEXPAND);
	return 0;
}


static void setupalias(Lex_t *lp, const char *string,Namval_t *np)
{
	Sfio_t *iop, *base;
	struct alias *ap = (struct alias*)sh_malloc(sizeof(struct alias));
	ap->disc = alias_disc;
	ap->lp = lp;
	ap->buf[1] = 0;
	if(ap->np = np)
	{
#if SHOPT_KIA
		if(kia.file)
		{
			unsigned long r;
			r=kiaentity(lp,nv_name(np),-1,'p',0,0,kia.current,'a',0,"");
			sfprintf(kia.tmp,"p;%..64d;p;%..64d;%d;%d;e;\n",kia.current,r,sh.inlineno,sh.inlineno);
		}
#endif /* SHOPT_KIA */
		if((ap->nextc=fcget())==0)
			ap->nextc = ' ';
	}
	else
		ap->nextc = 0;
	iop = sfopen(NULL,(char*)string,"s");
	sfdisc(iop, &ap->disc);
	lp->lexd.nocopy++;
	if(!(base=fcfile()))
		base = sfopen(NULL,fcseek(0),"s");
	fcclose();
	sfstack(base,iop);
	fcfopen(base);
	lp->lexd.nocopy--;
}

/*
 * grow storage stack for nested constructs by STACK_ARRAY
 */
static int stack_grow(void)
{
	lex_max += STACK_ARRAY;
	lex_match = sh_realloc(lex_match,sizeof(int)*lex_max);
	return 1;
}
