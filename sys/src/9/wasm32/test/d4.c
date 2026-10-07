#include <u.h>
#include <libc.h>
#include <mp.h>
#include <libsec.h>
#include <auth.h>

/*
 * (tools/test/run) D4's TLS: devtls (#a, /net/tls) and libsec's
 * handshake on the wasm32 kernel - a client and a server over a pipe
 * with a pre-shared key, as rcpu's tlsclient -a and tlssrv -a use the
 * dp9ik secret (pskID p9secret): a line each way, then a megabyte from
 * the client, its sha256 the same on both ends; a server with another
 * key refuses the client.
 *
 * d4 -s addr: an rcpu server as 9front's /rc/bin/service/tcp17019 is one
 * (tlssrv -a, then rc reads the client's script), for one client that
 * comes to addr (announce, listen) - the machine's own rcpu, unchanged,
 * over its own IP address: p9any as the server (user bootes), TLS with
 * the dp9ik secret, the script on 0 and 1.
 *
 * d4 -a: /proc/n/args written and read at once, by procs that come and
 * go - devproc takes the proc's debug lock and looks at its pid under
 * it (the review's: a write could free args under a reader, or land in
 * a slot made again for another proc).
 *
 * d4 -k: exportfs's ending - rfork(RFMEM) procs waiting in rendezvous,
 * their note handler noted(NDFLT) on kill, end when their group gets
 * kill (exportfs's fatal: postnote(PNGROUP, ...)); none left after.
 */

enum { Big = 1024*1024 };

static uchar psk[32];

static int
tlsend(int fd, int server, uchar *key)
{
	TLSconn *c;
	int r;

	c = mallocz(sizeof *c, 1);
	c->pskID = "p9secret";
	c->psk = key;
	c->psklen = sizeof psk;
	r = server ? tlsServer(fd, c) : tlsClient(fd, c);
	free(c->cert);
	free(c->sessionID);
	free(c);
	return r;
}

static void
sum(char *who, uchar *p, int n)
{
	uchar d[SHA2_256dlen];

	sha2_256(p, n, d, nil);
	print("%s: %d bytes, sha256 %.*H\n", who, n, 8, d);
}

static void
loop(void)
{
	int p[2], fd, n, m;
	uchar *big;
	char buf[128];

	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	switch(fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		close(p[0]);
		if((fd = tlsend(p[1], 1, psk)) < 0)
			sysfatal("server: %r");
		if((n = read(fd, buf, sizeof buf-1)) <= 0)
			sysfatal("server read: %r");
		buf[n] = 0;
		print("server got: %s", buf);
		fprint(fd, "hello from the server\n");
		big = malloc(Big);
		for(n = 0; n < Big; n += m)
			if((m = read(fd, big+n, Big-n)) <= 0)
				sysfatal("server read big: %r");
		sum("server", big, n);
		write(fd, "x", 1);	/* done: the client may go */
		exits(nil);
	}
	close(p[1]);
	if((fd = tlsend(p[0], 0, psk)) < 0)
		sysfatal("client: %r");
	fprint(fd, "hello from the client\n");
	if((n = read(fd, buf, sizeof buf-1)) <= 0)
		sysfatal("client read: %r");
	buf[n] = 0;
	print("client got: %s", buf);
	big = malloc(Big);
	for(n = 0; n < Big; n++)
		big[n] = n*13 + (n>>8);
	sum("client", big, Big);	/* before the server's: it sums what it gets */
	if(write(fd, big, Big) != Big)
		sysfatal("client write: %r");
	read(fd, buf, 1);
	waitpid();
	close(fd);
}

static int
pipenote(void*, char *msg)
{
	/*
	 * the other end gone (a server that refuses, or done): devtls's
	 * close_notify, or the next write, gets the pipe's note
	 */
	return strstr(msg, "write on closed pipe") != nil;
}

static void
wrongkey(void)
{
	int p[2], fd;
	uchar other[32];

	memset(other, 0x77, sizeof other);
	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	switch(fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		close(p[0]);
		fd = tlsend(p[1], 1, other);
		close(p[1]);
		exits(fd < 0 ? "refused" : nil);
	}
	close(p[1]);
	fd = tlsend(p[0], 0, psk);
	if(fd >= 0){
		/* the handshake's Finished differs: the first record fails */
		fprint(fd, "x");
		if(read(fd, psk, 1) > 0)
			print("wrong key: taken\n");
		close(fd);
	}
	close(p[0]);
	waitpid();
	print("wrong key: refused\n");
}

static void
rcpuserver(char *addr)
{
	int afd, lfd, fd;
	char adir[40], ldir[40];
	AuthInfo *ai;
	TLSconn *c;

	/*
	 * its own note group and namespace, as aux/listen's services: rcpu's
	 * server script posts the client's interrupt and hangup to its group,
	 * which the client in this one machine must not get back, and mounts
	 * the connection on /mnt/term, which must go when the session does
	 */
	rfork(RFNOTEG|RFNAMEG);
	if((afd = announce(addr, adir)) < 0)
		sysfatal("announce %s: %r", addr);
	if((lfd = listen(adir, ldir)) < 0)
		sysfatal("listen %s: %r", adir);
	if((fd = accept(lfd, ldir)) < 0)
		sysfatal("accept %s: %r", ldir);
	close(lfd);
	close(afd);
	/* tlssrv -a */
	if((ai = auth_proxy(fd, nil, "proto=p9any role=server user=bootes")) == nil)
		sysfatal("auth: %r");
	c = mallocz(sizeof *c, 1);
	c->pskID = "p9secret";
	c->psk = ai->secret;
	c->psklen = ai->nsecret;
	if((fd = tlsServer(fd, c)) < 0)
		sysfatal("tls: %r");
	dup(fd, 0);
	dup(fd, 1);
	close(fd);
	/* tcp17019's server */
	execl("/bin/rc", "rc", "-c", ". <{n=`{read} && ! ~ $#n 0 && read -c $n} >[2=1]", nil);
	sysfatal("exec: %r");
}

static void
slavenote(void*, char *msg)
{
	if(strncmp(msg, "kill", 4) == 0)
		noted(NDFLT);
	noted(NCONT);
}

static void
killgroup(int flags, int handler)
{
	int i, pid, fd, n, left;
	char buf[64], path[32], *f[3];
	int pids[3];

	switch(pid = rfork(RFPROC|RFFDG)){
	case -1:
		sysfatal("fork: %r");
	case 0:
		rfork(RFNOTEG|RFREND);
		for(i = 0; i < nelem(pids); i++){
			switch(pids[i] = rfork(flags)){
			case -1:
				sysfatal("rfork: %r");
			case 0:
				if(handler)
					notify(slavenote);
				for(;;)
					rendezvous(&pids[i], nil);
			}
		}
		sleep(500);
		print("%d procs waiting in rendezvous\n", nelem(pids));
		/* the pids, for the parent to look for */
		fd = create("/env/killpids", OWRITE, 0666);
		fprint(fd, "%d %d %d", pids[0], pids[1], pids[2]);
		close(fd);
		postnote(PNGROUP, getpid(), "kill");
		exits(nil);
	}
	waitpid();
	sleep(1000);
	fd = open("/env/killpids", OREAD);
	n = read(fd, buf, sizeof buf-1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	left = 0;
	n = tokenize(buf, f, nelem(f));
	for(i = 0; i < n; i++){
		snprint(path, sizeof path, "/proc/%s/status", f[i]);
		if(access(path, AEXIST) == 0)
			left++;
	}
	print("after kill: %d left\n", left);
}

static void
ignorenote(void*, char*)
{
	noted(NCONT);
}

static void
notestorm(int n)
{
	int i, pid;

	rfork(RFNOTEG);
	switch(pid = fork()){
	case 0:
		notify(ignorenote);
		for(;;)
			sleep(1000);
	}
	sleep(500);
	for(i = 0; i < n; i++)
		postnote(PNGROUP, getpid(), "interrupt");
	postnote(PNPROC, pid, "kill");
	print("%d notes posted\n", n);
}

static void
argsrace(void)
{
	int i, j, k, fd, pids[4], n;
	char path[32], buf[ERRMAX];

	for(k = 0; k < 20; k++){
		for(j = 0; j < nelem(pids); j++){
			switch(pids[j] = fork()){
			case -1:
				sysfatal("fork: %r");
			case 0:
				snprint(path, sizeof path, "/proc/%d/args", getpid());
				for(i = 0; i < 50; i++){
					if((fd = open(path, OWRITE)) >= 0){
						fprint(fd, "writer %d round %d %.*s", j, i, i, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
						close(fd);
					}
				}
				exits(nil);
			}
		}
		/* read theirs while they write and go */
		for(i = 0; i < 100; i++){
			snprint(path, sizeof path, "/proc/%d/args", pids[i%nelem(pids)]);
			if((fd = open(path, OREAD)) >= 0){
				n = read(fd, buf, sizeof buf-1);
				if(n > 0){
					buf[n] = 0;
					if(strncmp(buf, "writer ", 7) != 0 && strcmp(buf, "d4") != 0)
						print("args: read %q\n", buf);
				}
				close(fd);
			}
		}
		for(j = 0; j < nelem(pids); j++)
			waitpid();
	}
	print("args: ok\n");
}

void
main(int argc, char **argv)
{
	int i;

	if(argc == 2 && strcmp(argv[1], "-a") == 0){
		argsrace();
		exits(nil);
	}

	if(argc == 3 && strcmp(argv[1], "-N") == 0){
		notestorm(atoi(argv[2]));
		exits(nil);
	}

	if(argc == 2 && strcmp(argv[1], "-k") == 0){
		killgroup(RFPROC|RFMEM|RFNOWAIT, 1);	/* exportfs's slaves */
		exits(nil);
	}
	if(argc == 3 && strcmp(argv[1], "-k") == 0){	/* the others: m(em) n(o handler) */
		killgroup(RFPROC|RFNOWAIT|(strchr(argv[2], 'm') ? RFMEM : 0), strchr(argv[2], 'n') == nil);
		exits(nil);
	}

	if(argc == 3 && strcmp(argv[1], "-s") == 0){
		rcpuserver(argv[2]);
		exits(nil);
	}

	fmtinstall('H', encodefmt);
	atnotify(pipenote, 1);
	for(i = 0; i < sizeof psk; i++)
		psk[i] = i*7;
	loop();
	wrongkey();
	print("done\n");
	exits(nil);
}
