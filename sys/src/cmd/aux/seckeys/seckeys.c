/*
 * seckeys - factotum's keys from secstore, and a copy of them on the
 * machine's disk for when there is no secstore: the wasm32 machine's
 * /boot/secstore.
 *
 * The secstore password is asked once (readcons: not echoed) and goes to
 * the programs that need it through pipes, as their -i takes it:
 * auth/secstore -i -G factotum gets the keys from the server, each line
 * of them is written to /mnt/factotum/ctl, and auth/aescbc -e -i makes
 * the copy (file) from them - encrypted with the same password, with
 * aescbc's HMAC.  Without a server (the dial fails) auth/aescbc -d -i
 * takes the keys from the copy.  A wrong password is asked again, three
 * times; an empty one goes on without keys.  Neither the password nor
 * the keys in the clear touch the disk or the environment.
 *
 *	aux/seckeys [-s server] [-u user | -I] [file]
 *
 * server: -s, else $secstore, else $auth; none: the copy only.  No
 * file: no copy (a machine without a disk).  -u: the user's secstore
 * (secstore -u), else this machine's user's.  -I: the user and the
 * password from standard input, a line each (auth/passkey login's), not
 * asked; once.
 */
#include <u.h>
#include <libc.h>
#include <authsrv.h>

enum {
	Maxkeys	= 512*1024,
	Ntries	= 3,
};

static void
usage(void)
{
	fprint(2, "usage: %s [-s server] [-u user | -I] [file]\n", argv0);
	exits("usage");
}

/* all of fd's bytes into *buf (alloc'd): their number, or -1 */
static long
readall(int fd, char **buf)
{
	long n, m;

	*buf = malloc(Maxkeys);
	if(*buf == nil)
		return -1;
	for(n = 0; n < Maxkeys && (m = read(fd, *buf+n, Maxkeys-n)) > 0; n += m)
		;
	if(m > 0){	/* more than Maxkeys */
		free(*buf);
		*buf = nil;
		return -1;
	}
	return n;
}

/* the command's: the pipes' fds closed, but for those it has as 0, 1 and 3 */
static void
closeexcept(int *ppw, int *pin, int *pout, int pwfd)
{
	int i, fd[6];

	fd[0] = ppw[0], fd[1] = ppw[1], fd[2] = pin[0];
	fd[3] = pin[1], fd[4] = pout[0], fd[5] = pout[1];
	for(i = 0; i < 6; i++)
		if(fd[i] > 1 && (fd[i] != 3 || pwfd != 3))
			close(fd[i]);
}

/*
 * argv with pw on pwfd (0 or 3) and in on its standard input (pwfd 3),
 * its standard output in *out: its exit status (alloc'd; "" for success).
 * A process of its own writes what it reads, which it may read while it
 * writes
 */
static char*
run(char **argv, int pwfd, char *pw, char *in, long nin, char **out, long *nout)
{
	int ppw[2], pin[2], pout[2], pid;
	Waitmsg *w;
	char *s;

	*out = nil;
	*nout = 0;
	if(pipe(ppw) < 0 || pipe(pin) < 0 || pipe(pout) < 0)
		return smprint("pipe: %r");
	switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
	case -1:
		return smprint("fork: %r");
	case 0:
		close(ppw[0]);
		close(pin[0]);
		close(pout[0]);
		close(pout[1]);
		write(ppw[1], pw, strlen(pw));
		close(ppw[1]);
		if(nin > 0)
			write(pin[1], in, nin);
		close(pin[1]);
		_exits(nil);
	}
	switch(pid = fork()){
	case -1:
		return smprint("fork: %r");
	case 0:
		/* a pipe's fd may be 3 already: what goes to 0, 1 and 3 is not closed after */
		dup(pwfd == 0 ? ppw[0] : pin[0], 0);
		dup(pout[1], 1);
		if(pwfd == 3)
			dup(ppw[0], 3);
		closeexcept(ppw, pin, pout, pwfd);
		exec(argv[0], argv);
		_exits(smprint("exec %s: %r", argv[0]));
	}
	close(ppw[0]);
	close(ppw[1]);
	close(pin[0]);
	close(pin[1]);
	close(pout[1]);
	*nout = readall(pout[0], out);
	close(pout[0]);
	while((w = wait()) != nil){
		if(w->pid != pid){
			free(w);
			continue;
		}
		s = strdup(*nout < 0 ? "too many keys" : w->msg);
		free(w);
		return s;
	}
	return smprint("wait: %r");
}

/* the keys to factotum, a line at a time, as factotum's ctl takes them */
static int
load(char *keys, long n)
{
	int fd, nk;
	char *p, *e, *nl;

	if((fd = open("/mnt/factotum/ctl", OWRITE)) < 0){
		fprint(2, "%s: /mnt/factotum/ctl: %r\n", argv0);
		return -1;
	}
	nk = 0;
	for(p = keys, e = keys+n; p < e; p = nl+1){
		if((nl = memchr(p, '\n', e-p)) == nil)
			nl = e;
		if(nl > p && write(fd, p, nl-p) == nl-p)
			nk++;
	}
	close(fd);
	return nk;
}

/* the copy: the keys encrypted with pw, by aescbc, put in place whole */
static void
savecopy(char *file, char *pw, char *keys, long n)
{
	char *argv[] = { "/bin/auth/aescbc", "-e", "-i", nil };
	char *st, *enc, *tmp, *base;
	long nenc;
	int fd;
	Dir d;

	st = run(argv, 3, pw, keys, n, &enc, &nenc);
	if(st[0] != 0 || nenc <= 0){
		fprint(2, "%s: no copy of the keys: aescbc: %s\n", argv0, st);
		goto out;
	}
	tmp = smprint("%s.new", file);
	if((fd = create(tmp, OWRITE, 0600)) < 0){
		fprint(2, "%s: no copy of the keys: %r\n", argv0);
		free(tmp);
		goto out;
	}
	if(write(fd, enc, nenc) != nenc){
		fprint(2, "%s: no copy of the keys: %s: %r\n", argv0, tmp);
		close(fd);
		remove(tmp);
		free(tmp);
		goto out;
	}
	close(fd);
	remove(file);
	base = strrchr(file, '/');
	nulldir(&d);
	d.name = base != nil ? base+1 : file;
	if(dirwstat(tmp, &d) < 0)
		fprint(2, "%s: no copy of the keys: %s: %r\n", argv0, tmp);
	free(tmp);
out:
	free(st);
	if(enc != nil){
		memset(enc, 0, nenc);
		free(enc);
	}
}

void
main(int argc, char **argv)
{
	char *server, *file, *pw, *st, *keys, *copy;
	char *sargv[] = { "/bin/auth/secstore", "-i", "-G", "factotum", "-s", nil, nil, nil, nil };
	char *user, *in, *nl;
	int fromstdin, ntries;
	char *dargv[] = { "/bin/auth/aescbc", "-d", "-i", nil };
	long nkeys, ncopy;
	int fd, try, nk;

	server = nil;
	user = nil;
	fromstdin = 0;
	ARGBEGIN{
	case 's':
		server = EARGF(usage());
		break;
	case 'u':
		user = EARGF(usage());
		break;
	case 'I':
		fromstdin = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc > 1 || fromstdin && user != nil)
		usage();
	in = nil;
	ntries = Ntries;
	if(fromstdin){
		/* user, then password: a line each */
		if(readall(0, &in) <= 0 || (nl = strchr(in, '\n')) == nil)
			sysfatal("no user and password on standard input");
		*nl++ = 0;
		user = in;
		in = nl;
		if((nl = strchr(in, '\n')) != nil)
			*nl = 0;
		ntries = 1;
	}
	file = argc == 1 ? argv[0] : nil;
	if(server == nil && (server = getenv("secstore")) == nil)
		server = getenv("auth");
	if(server != nil && *server == 0)
		server = nil;

	/* the copy, read before the password is asked: none and no server, nothing to ask for */
	copy = nil;
	ncopy = -1;
	if(file != nil && (fd = open(file, OREAD)) >= 0){
		ncopy = readall(fd, &copy);
		close(fd);
	}
	if(server == nil && ncopy <= 0)
		exits(nil);
	sargv[5] = server;
	if(user != nil){
		sargv[6] = "-u";
		sargv[7] = user;
	}

	for(try = 0; try < ntries; try++){
		pw = fromstdin ? strdup(in) : readcons("secstore password", nil, 1);
		if(pw == nil || pw[0] == 0){
			fprint(2, "%s: no password: no keys\n", argv0);
			exits(nil);
		}
		keys = nil;
		nkeys = 0;
		st = strdup("dial failed");
		if(server != nil){
			free(st);
			st = run(sargv, 0, pw, nil, 0, &keys, &nkeys);
		}
		if(st[0] == 0){
			nk = load(keys, nkeys);
			print("%s: %d keys from secstore\n", argv0, nk);
			if(file != nil)
				savecopy(file, pw, keys, nkeys);
		}else if(strstr(st, "dial failed") != nil && ncopy > 0){
			free(keys);
			keys = nil;
			free(st);
			st = run(dargv, 3, pw, copy, ncopy, &keys, &nkeys);
			if(st[0] == 0){
				nk = load(keys, nkeys);
				print("%s: no secstore: %d keys from the copy\n", argv0, nk);
			}else if(strstr(st, "authenticate") == nil){
				fprint(2, "%s: the copy: %s\n", argv0, st);
				try = ntries;
			}
		}else if(strstr(st, "invalid password") == nil){
			fprint(2, "%s: secstore: %s\n", argv0, st);
			try = ntries;
		}
		memset(pw, 0, strlen(pw));
		free(pw);
		if(keys != nil){
			memset(keys, 0, nkeys);
			free(keys);
		}
		if(st[0] == 0)
			exits(nil);
		free(st);
		if(try < ntries)
			fprint(2, "%s: wrong password\n", argv0);
	}
	exits("no keys");
}
