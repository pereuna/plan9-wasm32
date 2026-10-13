/*
 * APE on wasm32 (tools/test/run ape): main's argv and environment
 * (ap/wasm32/main9.c), stdio's printf, setjmp, a signal - a note, its
 * handler (notetramp.c) - left with siglongjmp (<setjmp.h>'s sigsetjmp),
 * fork and wait
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/wait.h>

static jmp_buf jb;
static sigjmp_buf sjb;
static volatile int caught;

static void
jump(int n)
{
	longjmp(jb, n);
}

static void
onint(int sig)
{
	caught = sig;
	siglongjmp(sjb, 7);
}

int
main(int argc, char **argv)
{
	int i, st, r;
	pid_t pid;

	for(i = 0; i < argc; i++)
		printf("argv[%d] %s\n", i, argv[i]);
	printf("%d %ld %lld %.3f %s\n", 42, -7L, 1LL<<40, 3.25, getenv("apetest") ? getenv("apetest") : "nil");
	printf("pid %s\n", getpid() > 0 ? "ok" : "bad");
	if((r = setjmp(jb)) == 0)
		jump(5);
	printf("setjmp %d\n", r);
	signal(SIGINT, onint);
	if((r = sigsetjmp(sjb, 1)) == 0){
		kill(getpid(), SIGINT);
		for(;;)
			sleep(1);
	}
	printf("sigsetjmp %d, signal %s\n", r, caught == SIGINT ? "SIGINT" : "none");
	fflush(stdout);	/* or the child's exit writes it again */
	pid = fork();
	if(pid == 0)
		exit(3);
	if(waitpid(pid, &st, 0) != pid)
		printf("waitpid failed\n");
	printf("child exit %d\n", WEXITSTATUS(st));
	return 0;
}
