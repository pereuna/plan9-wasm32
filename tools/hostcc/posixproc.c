/*
 * The native cc's (tools/build-cc native) fork, exec and wait, for -p:
 * cc runs the C preprocessor in a child (cc/lex.c).  POSIX's own, apart
 * from cc9's headers; $CPP names the preprocessor in place of cc's /bin/cpp
 * (tools/native-wasm32: 9front's cpp built for the host).  The Emscripten
 * build has none: one file per run.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int
ccfork(void)
{
	return fork();
}

int
ccexec(char *path, char *argv[])
{
	char *p;

	if(strcmp(path, "/bin/cpp") == 0 && (p = getenv("CPP")) != NULL && *p)
		path = p;
	return execv(path, argv);
}

int
ccwait(int *status)
{
	int s, pid;

	pid = wait(&s);
	if(status != NULL)
		*status = WIFEXITED(s) ? WEXITSTATUS(s) : 1;
	return pid;
}
