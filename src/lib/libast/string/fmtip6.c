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

#include <ast.h>
#include <ip6.h>

/*
 * copy p to s, then convert 0<=n<=999 to text
 * next char in s returned
 * caller ensures that s can take strlen(p)+3 bytes
 */

static char*
dec(char* s, char* p, int n)
{
	while (*s = *p++)
		s++;
	if (n >= 100)
		*s++ = '0' + ((n / 100) % 10);
	if (n >= 10)
		*s++ = '0' + ((n / 10) % 10);
	*s++ = '0' + (n % 10);
	return s;
}

/*
 * return pointer to normalized ipv6 address addr
 * with optional prefix bits if 0 <= bits <= 128
 * return value in short-term circular buffer
 */

char*
fmtip6(const unsigned char* addr, int bits)
{
	const unsigned char*	a = addr;
	int			n = IP6ADDR;
	int			i;
	int			z;
	int			k;
	int			m;
	unsigned char		r[IP6ADDR];
	char*			b;
	char*			s;

	static const char	dig[] = "0123456789ABCDEF";

	s = b = fmtbuf(44);
	r[m = z = 0] = 0;
	if (a[0] == 0x20 && a[1] == 0x02 && (a[2] || a[3] || a[4] || a[5]))
	{
		z = 6;
		s = dec(s, "2002:", a[2]);
		s = dec(s, ".", a[3]);
		s = dec(s, ".", a[4]);
		s = dec(s, ".", a[5]);
	}
	for (i = z; i < n; i += 2)
	{
		for (k = i; i < n - 1 && !a[i] && !a[i + 1]; i += 2);
		if ((r[k] = i - k) > r[m] || r[k] == r[m] && i >= (n - 1))
			m = k;
	}
	if (!m)
		switch (r[m])
		{
		case 0:
			m = -1;
			break;
		case 14:
			if (!a[14] && a[15] <= 15)
				break;
			/* FALLTHROUGH */
		case 12:
			s = dec(s, "::", a[12]);
			s = dec(s, ".", a[13]);
			s = dec(s, ".", a[14]);
			s = dec(s, ".", a[15]);
			n = 0;
			break;
		case 10:
			if (a[10] == 0xFF && a[11] == 0xFF)
			{
				s = dec(s, "::FFFF:", a[12]);
				s = dec(s, ".", a[13]);
				s = dec(s, ".", a[14]);
				s = dec(s, ".", a[15]);
				n = 0;
			}
			break;
		}
	for (i = z; i < n; i++)
	{
		if (i == m)
		{
			*s++ = ':';
			*s++ = ':';
			if ((i += r[m]) >= n)
			{
				z = 1;
				break;
			}
			z = 0;
		}
		else if (i && !(i & 1))
		{
			if (z)
				z = 0;
			else
				*s++ = '0';
			*s++ = ':';
		}
		if ((k = (a[i] >> 4) & 0xf) || z)
		{
			z = 1;
			*s++ = dig[k];
		}
		if ((k = a[i] & 0xf) || z)
		{
			z = 1;
			*s++ = dig[k];
		}
	}
	if (!z && *(s - 1) == ':')
		*s++ = '0';
	if (bits >= 0 && bits <= 128)
		s = dec(s, "/", bits);
	*s = 0;
	return b;
}
