/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1982-2011 AT&T Intellectual Property          *
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
#ifndef S_BREAK
#define S_BREAK	1	/* end of token */
#define S_EOF	2	/* end of buffer */
#define S_NL	3	/* new-line when not a token */
#define S_RES	4	/* first character of reserved word */
#define S_NAME	5	/* other identifier characters */
#define S_REG	6	/* non-special characters */
#define S_TILDE	7	/* first char is tilde */
#define S_PUSH	8
#define S_POP	9
#define S_BRACT	10
#define S_LIT	11	/* literal quote character */
#define S_NLTOK	12	/* new-line token */
#define S_OP	13	/* operator character */
#define S_PAT	14	/* pattern characters * and ? */
#define S_EPAT	15	/* pattern char when followed by ( */
#define S_EQ	16	/* assignment character */
#define S_COM	17	/* comment character */
#define S_MOD1	18	/* ${...} modifier character - old quoting */
#define S_MOD2	19	/* ${...} modifier character - new quoting */
#define S_ERR	20	/* invalid character in ${...} */
#define S_SPC1	21	/* special prefix characters after $ */
#define S_SPC2	22	/* special characters after $ */
#define S_DIG	23	/* digit character after $ */
#define S_ALP	24	/* alphabetic character after $ */
#define S_LBRA	25	/* left brace after $ */
#define S_RBRA	26	/* right brace after $ */
#define S_PAR	27	/* set for $( */
#define S_ENDCH	28	/* macro expansion terminator */
#define S_SLASH	29	/* / character terminates ~ expansion */
#define S_COLON	30	/* for character : */
#define S_EDOL	32	/* ends $identifier */
#define S_BRACE	33	/* left brace */
#define S_DOT	34	/* . char */
#define S_META	35	/* | & ; < > inside ${...} reserved for future use */
#define S_SPACE	S_BREAK	/* IFS space characters */
#define S_DELIM	S_RES	/* IFS delimiter characters */
#define S_MBYTE S_NAME	/* IFS first byte of multi-byte char */
#define S_BLNK	36	/* space or tab */
#define S_BRAOP	S_RES	/* potentially a glob pattern bracket expression operator (!, ^, -) */
/* The following must be the highest numbered states */
#define S_QUOTE	37	/* double quote character */
#define S_GRAVE	38	/* old comsub character */
#define S_ESC	39	/* escape character */
#define S_DOL	40	/* $ substitution character */
#define S_ESC2	41	/* escape character inside '...' */

/* These are the lexical state table names */
/* See lexstates.c  (ST_NONE must be last) */
#define ST_BEGIN	0
#define ST_NAME		1
#define ST_NORM		2
#define ST_LIT		3
#define ST_QUOTE	4
#define ST_NESTED	5
#define ST_DOL		6
#define ST_BRACE	7
#define ST_DOLNAME	8
#define ST_MACRO	9
#define ST_QNEST	10
#define ST_MOD1		11
#define ST_NONE		12

#include "FEATURE/locale"

#if _hdr_wchar
#   include <wchar.h>
#   if _hdr_wctype
#       include <wctype.h>
#       undef  isalpha
#       define isalpha(x)      iswalpha(x)
#       if defined(iswblank) || _lib_iswblank
#           undef  isblank
#           define isblank(x)      iswblank(x)
#       else
#           if _lib_wctype && _lib_iswctype
#               define _lib_iswblank	-1
#               undef  isblank
#	        define isblank(x)	local_iswblank(x)
	        extern int		local_iswblank(wchar_t);
#           endif
#       endif
#   endif
#endif
#ifndef isblank
#   define isblank(x)      ((x)==' '||(x)=='\t')
#endif

#undef LEN
#undef SETLEN
#if SHOPT_MULTIBYTE
#   define LEN		_Fcin.fclen
#   define SETLEN(x)	(_Fcin.fclen = x)
#   define isaname(c)	((c) < 0 ? 0 : ((c) > 0x7f ? isalpha(c) : sh_lexstates[ST_NAME][(c)] == 0))
#   define isaletter(c)	((c) < 0 ? 0 : ((c) > 0x7f ? isalpha(c) : sh_lexstates[ST_DOL][(c)] == S_ALP && (c) != '.'))
#else
#   undef mbwide
#   define mbwide()	(0)
#   define LEN		1
#   define SETLEN(x)	(x)
#   define isaname(c)	(((c) < 0 || (c) > 255) ? 0 : sh_lexstates[ST_NAME][c] == 0)
#   define isaletter(c)	(((c) < 0 || (c) > 255) ? 0 : (sh_lexstates[ST_DOL][c] == S_ALP && (c) != '.'))
#endif
#define STATE(s,c)	(s[mbwide() ? ((c = fcmbget(&LEN)), LEN > 1 ? 'a' : c) : (c = fcget())])
#define isadigit(c)	(((c) < 0 || (c) > 255) ? 0 : sh_lexstates[ST_DOL][c] == S_DIG)
#define isastchar(c)	((c) == '@' || (c) == '*')
#define isexp(c)	(((c) < 0 || (c) > 255) ? 0 : (sh_lexstates[ST_MACRO][c] == S_PAT || (c) == '$' || (c) == '`'))
#define ismeta(c)	(((c) < 0 || (c) > 255) ? 0 : sh_lexstates[ST_NAME][c] == S_BREAK)

extern const char *sh_lexstates[ST_NONE];
extern const char e_lexversion[];
extern const char e_lexspace[];
extern const char e_lexslash[];

extern const char e_syntaxerror[];
extern const char e_syntaxerror_at[];
extern const char e_unexpected[];
extern const char e_unmatched[];
extern const char e_emptysubscr[];
extern const char e_badreflist[];
extern const char e_heredoccomsub[];
extern const char e_lexzerobyte[];
extern const char e_endoffile[];
extern const char e_newline[];

extern const char e_lexwarnvar[];
extern const char e_lexarithwarn[];
extern const char e_lexobsolete1[];
extern const char e_lexobsolete2[];
extern const char e_lexobsolete3[];
extern const char e_lexobsolete4[];
extern const char e_lexobsolete5[];
extern const char e_lexobsolete6[];
extern const char e_lexusebrace[];
extern const char e_lexusequote[];
extern const char e_lexescape[];
extern const char e_lexquote[];
extern const char e_lexnested[];
extern const char e_lexbadchar[];
extern const char e_lexlongquote[];
extern const char e_lexfuture[];
extern const char e_lexemptyfor[];
extern const char e_lextypeset[];
extern const char e_lexcharclass[];
#endif
