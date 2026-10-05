/*
 * cc's compat functions (../third_party/9cc/cc/compat.h) on POSIX, in place
 * of cc/compat, which replaces malloc with brk.  One file per run: no fork,
 * but for the native build's -p (CCPROC: posixproc.c).
 */
#include "cc.h"

int
myaccess(char *f)
{
	return access(f, F_OK);
}

int
mycreat(char *n, int p)
{
	return create(n, OWRITE, p);
}

#ifdef CCPROC
int ccfork(void), ccexec(char*, char**), ccwait(int*);
#endif

int
mywait(int *s)
{
#ifdef CCPROC
	return ccwait(s);
#else
	USED(s);
	return -1;
#endif
}

int
mydup(int f1, int f2)
{
	return dup(f1, f2);
}

int
mypipe(int *fd)
{
	return pipe(fd);
}

/* Plan 9's file system layout: the headers are at /$objtype/include and /sys/include */
int
systemtype(int sys)
{
	return sys & Plan9;
}

int
pathchar(void)
{
	return '/';
}

char*
mygetwd(char *path, int len)
{
	return getwd(path, len);
}

int
myexec(char *path, char *argv[])
{
#ifdef CCPROC
	return ccexec(path, argv);
#else
	USED(path, argv);
	return -1;
#endif
}

int
myfork(void)
{
#ifdef CCPROC
	return ccfork();
#else
	return -1;
#endif
}

void*
alloc(long n)
{
	void *p;

	if((p = calloc(1, n)) == nil)
		sysfatal("out of memory");
	return p;
}

void*
allocn(void *p, long on, long n)
{
	if((p = realloc(p, on+n)) == nil)
		sysfatal("out of memory");
	memset((char*)p+on, 0, n);
	return p;
}
