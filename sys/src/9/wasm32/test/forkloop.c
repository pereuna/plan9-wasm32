#include <u.h>
#include <libc.h>

/*
 * fork, exit and wait, n times (tools/test-9wasm32): a dead proc's Proc
 * and KSTACK go to the next fork only when its Worker has left them.
 * Each child changes its directory; every other one execs this program
 * again, which exits with its argument.
 *	forkloop [n]
 */
void
main(int argc, char **argv)
{
	int i, n, pid, bad;
	char want[32];
	Waitmsg *w;

	if(argc == 3 && strcmp(argv[1], "-x") == 0)
		exits(argv[2]);
	n = argc > 1 ? atoi(argv[1]) : 1000;
	bad = 0;
	for(i = 0; i < n; i++){
		switch(pid = fork()){
		case -1:
			sysfatal("fork %d: %r", i);
		case 0:
			if(chdir(i % 3 ? "/bin" : "/rc/lib") < 0)
				exits("chdir");
			snprint(want, sizeof want, "c%d", i);
			if(i % 2)
				execl("/bin/forkloop", "forkloop", "-x", want, nil);
			exits(want);
		}
		if((w = wait()) == nil)
			sysfatal("wait %d: %r", i);
		snprint(want, sizeof want, ": c%d", i);
		if(w->pid != pid || strstr(w->msg, want) == nil){
			if(bad++ < 5)
				print("forkloop %d: pid %d msg '%s', want %d '%s'\n", i, w->pid, w->msg, pid, want);
		}
		free(w);
	}
	print("forkloop %d: %s\n", n, bad ? "wrong" : "ok");
	exits(bad ? "wrong" : nil);
}
