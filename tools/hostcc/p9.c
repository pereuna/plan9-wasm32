/* the Plan 9 system calls of libc.h, on POSIX */
#include <u.h>
#include <libc.h>
#undef dup

int
p9create(char *name, int mode, ulong perm)
{
	int m;

	m = O_CREAT|O_TRUNC;
	switch(mode&3){
	case OREAD:	m |= O_RDONLY; break;
	case OWRITE:	m |= O_WRONLY; break;
	default:	m |= O_RDWR; break;
	}
	return open(name, m, perm&0777);
}

vlong
p9seek(int fd, vlong off, int whence)
{
	return lseek(fd, off, whence);
}

void
p9exits(char *s)
{
	exit(s != nil && *s != 0);
}

char *argv0;

static char errbuf[ERRMAX];

void
werrstr(char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsnprint(errbuf, sizeof errbuf, fmt, arg);
	va_end(arg);
	errno = 0;
}

int
p9errstr(char *buf, uint n)
{
	if(errno != 0)
		snprint(buf, n, "%s", strerror(errno));
	else
		snprint(buf, n, "%s", errbuf);
	return 0;
}

int
p9dup(int a, int b)
{
	return b < 0 ? dup(a) : dup2(a, b);
}

char*
p9getwd(char *buf, int n)
{
	return getcwd(buf, n);
}

/* one thread: fmt needs no lock */
void
__fmtlock(void)
{
}

void
__fmtunlock(void)
{
}

void
sysfatal(char *fmt, ...)
{
	char buf[256];
	va_list arg;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	fprint(2, "%s\n", buf);
	exits("fatal");
}

#undef atexit
int
libatexit(void (*f)(void))
{
	return atexit(f) == 0;
}

/* %r */
int
__errfmt(Fmt *f)
{
	char buf[ERRMAX];

	errstr(buf, sizeof buf);
	return fmtstrcpy(f, buf);
}
