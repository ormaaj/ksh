/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1992-2012 AT&T Intellectual Property          *
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
*                  Martijn Dekker <martijn@inlv.org>                   *
*            Johnothan King <johnothanking@protonmail.com>             *
*                                                                      *
***********************************************************************/
/*
 * David Korn
 * Glenn Fowler
 * AT&T Research
 *
 * uname
 */

static const char usage[] =
"[-?\n@(#)$Id: uname (ksh 93u+m) 2023-03-09 $\n]"
"[--catalog?" ERROR_CATALOG "]"
"[+NAME?uname - identify the current system ]"
"[+DESCRIPTION?By default \buname\b writes the operating system name to"
"	standard output. When options are specified, one or more"
"	system characteristics are written to standard output, space"
"	separated, on a single line. When more than one option is specified"
"	the output is in the order specified by the \b-A\b option below."
"	Unsupported option values are listed as \a[option]]\a. If any unknown"
"	options are specified, the OS default \buname\b(1) is called (unless"
"	the shell is in restricted mode, in which case an error will occur).]"
"[+?If any \aname\a operands are specified then the \bsysinfo\b(2) values"
"	for each \aname\a are listed, separated by space, on one line."
"	\bgetconf\b(1), a pre-existing \astandard\a interface, provides"
"	access to the same information; vendors should spend more time"
"	using standards than inventing them.]"
"[+?Selected information is printed in the same order as the options below.]"
"[a:all?Equivalent to \b-snrvmpio\b.]"
"[s:system|sysname|kernel-name?The detailed kernel name. This is the default.]"
"[n:nodename?The hostname or nodename.]"
"[r:release|kernel-release?The kernel release level.]"
"[v:version|kernel-version?The kernel version level.]"
"[m:machine?The name of the hardware type the system is running on.]"
"[p:processor?The name of the processor instruction set architecture.]"
"[i:implementation|platform|hardware-platform?The hardware implementation;"
"	this is \b--host-id\b on some systems.]"
"[o:operating-system?The generic operating system name.]"
"[h:host-id|id?The host ID in hex.]"
"[d:domain?The domain name returned by \agetdomainname\a(2).]"
"[R:extended-release?The extended release name.]"
"[A:everything?Equivalent to \b-snrvmpiohdR\b.]"
"[f:list?List all \bsysinfo\b(2) names and values, one per line.]"
"[S:sethost?Set the hostname or nodename to \aname\a. No output is"
"	written to standard output.]:[name]"
"\n"
"\n[ name ... ]\n"
"\n"
"[+SEE ALSO?\bhostname\b(1), \bgetconf\b(1), \buname\b(2),"
"	\bsysconf\b(3), \bsysinfo\b(2)]"
;

#include <cmd.h>
#include <ctype.h>
#include <stdio.h>
#include <proc.h>

#include "FEATURE/utsname"

#define MAXHOSTNAME	64

#if _lib_uname && _sys_utsname
# include <sys/utsname.h>
#endif

#ifndef HOSTTYPE
#define HOSTTYPE	"unknown"
#endif

static const char	hosttype[] = HOSTTYPE;

#if !_lib_uname || !_sys_utsname

struct utsname
{
	char*	sysname;
	char	nodename[MAXHOSTNAME];
	char*	release;
	char*	version;
	char*	machine;
};

int
uname(struct utsname* ut)
{
#ifdef HOSTTYPE
	char*		sys = 0;
	char*		arch = 0;

	if (*hosttype)
	{
		static char	buf[sizeof(hosttype)];

		strcpy(buf, hosttype);
		sys = buf;
		if (arch = strchr(sys, '.'))
		{
			*arch++ = 0;
			if (!*arch)
				arch = 0;
		}
		if (!*sys)
			sys = 0;
	}
#endif
#if _lib_gethostname
	if (gethostname(ut->nodename, sizeof(ut->nodename) - 1))
		return -1;
#else
	strncpy(ut->nodename, "local", sizeof(ut->nodename) - 1);
#endif
#ifdef HOSTTYPE
	if (!(ut->sysname = sys))
#endif
	ut->sysname = ut->nodename;
#ifdef HOSTTYPE
	if (!(ut->machine = arch))
#endif
	ut->machine = "";
	ut->release = "";
	ut->version = "";
	return 0;
}

#endif

#define OPT_system		(1<<0)
#define OPT_nodename		(1<<1)
#define OPT_release		(1<<2)
#define OPT_version		(1<<3)
#define OPT_machine		(1<<4)
#define OPT_processor		(1<<5)

#define OPT_STANDARD		6

#define OPT_implementation	(1<<6)
#define OPT_operating_system	(1<<7)

#define OPT_ALL			8

#define OPT_hostid		(1<<8)
#define OPT_vendor		(1<<9)
#define OPT_domain		(1<<10)
#define OPT_machine_type	(1<<11)
#define OPT_base		(1<<12)
#define OPT_extended_release	(1<<13)
#define OPT_extra		(1<<14)

#define OPT_TOTAL		15

#define OPT_all			(1L<<29)
#define OPT_total		(1L<<30)
#define OPT_standard		((1<<OPT_STANDARD)-1)

#ifndef HOSTTYPE
#define HOSTTYPE		"unknown"
#endif

#define extra(m)        do \
			{ \
				if ((char*)&ut.m[sizeof(ut.m)] > last) \
					last = (char*)&ut.m[sizeof(ut.m)]; \
			} while(0)

#define output(f,v,u)	do \
			{ \
				if ((flags&(f))&&(*(v)||(flags&(OPT_all|OPT_total))==OPT_all&&((f)&OPT_standard)||!(flags&(OPT_all|OPT_total)))) \
				{ \
					if (sep) \
						sfputc(sfstdout, ' '); \
					else \
						sep = 1; \
					if (*(v)) \
						sfputr(sfstdout, v, -1); \
					else \
						sfprintf(sfstdout, "[%s]", u); \
				} \
			} while (0)

int
b_uname(int argc, char** argv, Shbltin_t* context)
{
	long		flags = 0;
	int		sep = 0;
	int		n;
	char*		s;
	char*		t;
	char*		e;
	char*		sethost = 0;
	int		list = 0;
	struct utsname	ut;
	char		buf[257];

	cmdinit(argc, argv, context, ERROR_CATALOG, 0);
	for (;;)
	{
		switch (optget(argv, usage))
		{
		case 'a':
			flags |= OPT_all|((1L<<OPT_ALL)-1);
#ifdef __linux__
			flags |= OPT_implementation;
#endif
			continue;
		case 'b':
			flags |= OPT_base;
			continue;
		case 'c':
			flags |= OPT_vendor;
			continue;
		case 'd':
			flags |= OPT_domain;
			continue;
		case 'f':
			list = 1;
			continue;
		case 'h':
			flags |= OPT_hostid;
			continue;
		case 'i':
			flags |= OPT_implementation;
			continue;
		case 'm':
			flags |= OPT_machine;
			continue;
		case 'n':
			flags |= OPT_nodename;
			continue;
		case 'o':
			flags |= OPT_operating_system;
			continue;
		case 'p':
			flags |= OPT_processor;
			continue;
		case 'r':
			flags |= OPT_release;
			continue;
		case 's':
			flags |= OPT_system;
			continue;
		case 't':
			flags |= OPT_machine_type;
			continue;
		case 'v':
			flags |= OPT_version;
			continue;
		case 'x':
			flags |= OPT_extra;
			continue;
		case 'A':
			flags |= OPT_total|((1L<<OPT_TOTAL)-1);
			continue;
		case 'R':
			flags |= OPT_extended_release;
			continue;
		case 'S':
			sethost = opt_info.arg;
			continue;
		case ':':
			{
				char **new_argv = stkalloc(stkstd, (argc + 3) * sizeof(char*));
				new_argv[0] = "command";
				new_argv[1] = "-px";
				for (n = 0; n <= argc; n++)
					new_argv[n + 2] = argv[n];
				return sh_run(context, argc + 2, new_argv);
			}
		case '?':
			error(ERROR_usage(2), "%s", opt_info.arg);
			UNREACHABLE();
		}
		break;
	}
	argv += opt_info.index;
	if (error_info.errors || *argv && (flags || sethost) || sethost && flags)
	{
		error(ERROR_usage(2), "%s", optusage(NULL));
		UNREACHABLE();
	}
	if (sethost)
	{
#if _lib_sethostname
		if (sethostname(sethost, strlen(sethost) + 1))
#else
#ifdef	ENOSYS
		errno = ENOSYS;
#else
		errno = EPERM;
#endif
#endif
		error(ERROR_system(1), "%s: cannot set host name", sethost);
		UNREACHABLE();
	}
	else if (list)
		astconflist(sfstdout, NULL, ASTCONF_base|ASTCONF_defined|ASTCONF_lower|ASTCONF_quote|ASTCONF_matchcall, "CS|SI");
	else if (*argv)
	{
		e = &buf[sizeof(buf)-1];
		while (s = *argv++)
		{
			t = buf;
			*t++ = 'C';
			*t++ = 'S';
			*t++ = '_';
			while (t < e && (n = *s++))
				*t++ = islower(n) ? toupper(n) : n;
			*t = 0;
			sfprintf(sfstdout, "%s%c", *(t = astconf(buf, NULL, NULL)) ? t : *(t = astconf(buf+3, NULL, NULL)) ? t :  "unknown", *argv ? ' ' : '\n');
		}
	}
	else
	{
		s = buf;
		if (!flags)
			flags = OPT_system;
		memzero(&ut, sizeof(ut));
		if (uname(&ut) < 0)
		{
			error(ERROR_usage(2), "information unavailable");
			UNREACHABLE();
		}
		output(OPT_system, ut.sysname, "sysname");
		if (flags & OPT_nodename)
		{
#if !_mem_nodeext_utsname && _lib_gethostname
			if (sizeof(ut.nodename) > 9 || gethostname(s, sizeof(buf)))
#endif
			s = ut.nodename;
			output(OPT_nodename, s, "nodename");
		}
		output(OPT_release, ut.release, "release");
		output(OPT_version, ut.version, "version");
		output(OPT_machine, ut.machine, "machine");
		if (flags & OPT_processor)
		{
			s = NULL;
#ifdef __linux__
# ifdef UNAME_PROCESSOR
			if (!s)
			{
				size_t len = sizeof(buf) - 1;
				int ctl[] = {CTL_HW, UNAME_PROCESSOR};
				if (sysctl(ctl, 2, buf, &len, 0, 0) == 0)
					s = buf;
			}
# endif
			if (!s)
			{
				strcpy((s = buf), ut.machine);
				if (strcmp(s, "i686") == 0)
				{
					char line[1024];
					Sfio_t *io = sfopen(NULL, "/proc/cpuinfo", "r");
					if (io)
					{
						while (fgets(line, sizeof(line), io))
						{
							if (strncmp(line, "vendor_id", 9) == 0)
							{
								if (strstr(line, "AuthenticAMD"))
									s = "athlon";
								break;
							}
						}
						sfclose(io);
					}
				}
			}
#endif
			if (!s && !*(s = astconf("ARCHITECTURE", NULL, NULL)))
				s = ut.machine;
			output(OPT_processor, s, "processor");
		}
		if (flags & OPT_implementation)
		{
			s = NULL;
#ifdef __linux__
			if (!s)
			{
				strcpy((s = buf), ut.machine);
				if (s[0] == 'i' && s[2] == '8' && s[3] == '6' && s[4] == '\0')
					s[1] = '3';
			}
#endif
			if (!s && !*(s = astconf("PLATFORM", NULL, NULL)) && !*(s = astconf("HW_NAME", NULL, NULL)))
			{
				if (t = strchr(hosttype, '.'))
					t++;
				else
					t = (char*)hosttype;
				strncpy(s = buf, t, sizeof(buf) - 1);
			}
			output(OPT_implementation, s, "implementation");
		}
		if (flags & OPT_operating_system)
		{
			s = astconf("OPERATING_SYSTEM", NULL, NULL);
			if (!*s)
#ifdef _UNAME_os_DEFAULT
				s = _UNAME_os_DEFAULT;
#else
				s = ut.sysname;
#endif
			output(OPT_operating_system, s, "operating-system");
		}
		if (flags & OPT_extended_release)
		{
			s = astconf("RELEASE", NULL, NULL);
			output(OPT_extended_release, s, "extended-release");
		}
#if _mem_idnumber_utsname
		output(OPT_hostid, ut.idnumber, "hostid");
#else
		if (flags & OPT_hostid)
		{
			if (!*(s = astconf("HW_SERIAL", NULL, NULL)))
#if _lib_gethostid
				sfsprintf(s = buf, sizeof(buf), "%08x", gethostid());
#else
				/*NOP*/;
#endif
			output(OPT_hostid, s, "hostid");
		}
#endif
		if (flags & OPT_vendor)
		{
			s = astconf("HW_PROVIDER", NULL, NULL);
			output(OPT_vendor, s, "vendor");
		}
		if (flags & OPT_domain)
		{
			if (!*(s = astconf("SRPC_DOMAIN", NULL, NULL)))
			{
#if _lib_getdomainname
				getdomainname(buf, sizeof(buf));
				s = buf;
#else
				/*NOP*/;
#endif
			}
			output(OPT_domain, s, "domain");
		}
#if _mem_m_type_utsname
		s = ut.m_type;
#else
		s = astconf("MACHINE", NULL, NULL);
#endif
		output(OPT_machine_type, s, "m_type");
#if _mem_base_rel_utsname
		s = ut.base_rel;
#else
		s = astconf("BASE", NULL, NULL);
#endif
		output(OPT_base, s, "base_rel");
		if (flags & OPT_extra)
		{
			char*	last = (char*)&ut;

			extra(sysname);
			extra(nodename);
			extra(release);
			extra(version);
			extra(machine);
#if _mem_idnumber_utsname
			extra(idnumber);
#endif
#if _mem_m_type_utsname
			extra(m_type);
#endif
#if _mem_base_rel_utsname
			extra(base_rel);
#endif
			if (last < ((char*)(&ut + 1)))
			{
				s = t = last;
				while (s < (char*)(&ut + 1))
				{
					if (!(n = *s++))
					{
						if ((s - t) > 1)
						{
							if (sep)
								sfputc(sfstdout, ' ');
							else
								sep = 1;
							sfputr(sfstdout, t, -1);
						}
						t = s;
					}
					else if (!isprint(n))
						break;
				}
			}
		}
		if (sep)
			sfputc(sfstdout, '\n');
	}
	return error_info.errors;
}
