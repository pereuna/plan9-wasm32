#include <u.h>
#include <libc.h>

/*
 * D2 (tools/test-9wasm32): /net over WebSockets - dial the machine's
 * rcpu (webterm /17019): its p9any server speaks first, the ways it
 * authenticates; then the auth server (567) as authdial names it
 * (net!...!ticket: /net/cs), and a port webterm refuses
 */
/*
 * -l n p: p procs, each n times rcpu's greeting on a conversation made
 * again and again: never another's bytes in it (a closed or refused one
 * is the server's business - webterm closes some when they come fast,
 * and leaves some silent: an alarm ends the read)
 */
static int
onalarm(void*, char *msg)
{
	return strcmp(msg, "alarm") == 0;	/* a silent server: the read is interrupted */
}

static void
loop(int n, int p)
{
	char buf[256];
	int i, j, k, fd, bad;
	Waitmsg *w;

	for(j = 0; j < p; j++){
		if(fork() != 0)
			continue;
		bad = 0;
		atnotify(onalarm, 1);
		for(i = 0; i < n; i++){
			if((fd = dial("tcp!cpu!17019", nil, nil, nil)) < 0)
				continue;
			alarm(15000);
			for(k = 0; k < sizeof buf - 1 && read(fd, buf+k, 1) == 1 && buf[k] != 0; k++)
				;
			alarm(0);
			buf[k] = 0;
			if(k > 0 && strncmp(buf, "dp9ik@", 6) != 0 && strstr(buf, " dp9ik@") == nil)
				bad++;
			close(fd);
		}
		exits(bad ? "another's bytes" : nil);
	}
	bad = 0;
	for(j = 0; j < p; j++)
		if((w = wait()) == nil || w->msg[0] != 0)
			bad++;
	print("netloop %d x %d: %s\n", p, n, bad ? "another's bytes" : "no mixed data");
	exits(nil);
}

void
main(int argc, char **argv)
{
	char buf[256], dom[64], *p;
	int fd, n, i;

	if(argc == 4 && strcmp(argv[1], "-l") == 0)
		loop(atoi(argv[2]), atoi(argv[3]));

	if((fd = dial("tcp!cpu!17019", nil, nil, nil)) < 0)
		sysfatal("dial rcpu: %r");
	n = 0;
	while(n < sizeof buf - 1 && (i = read(fd, buf+n, 1)) == 1 && buf[n] != 0)
		n++;
	buf[n] = 0;
	print("rcpu says: %s\n", strstr(buf, "dp9ik@") != nil ? "p9any: dp9ik" : buf);
	/* dp9ik, and a client's challenge: the server's ticket request comes back */
	p = strchr(buf, '@');
	snprint(dom, sizeof dom, "dp9ik %s", p != nil ? p+1 : "");
	if(write(fd, dom, strlen(dom)+1) != strlen(dom)+1 || write(fd, "01234567", 8) != 8)
		sysfatal("write rcpu: %r");
	for(n = 0; n < 141 && (i = read(fd, buf, sizeof buf)) > 0; n += i)
		;
	print("rcpu answers the challenge: %s\n", n >= 141 ? "a ticket request" : "no");
	close(fd);
	/* through /net/cs; webterm closes some connections that come fast: a few tries */
	for(i = 0; i < 3 && (fd = dial("net!cpu!ticket", nil, nil, nil)) < 0; i++)
		sleep(1000);
	if(fd < 0)
		sysfatal("dial ticket: %r");
	print("auth: connected\n");
	close(fd);
	if((fd = dial("tcp!cpu!25", nil, nil, nil)) >= 0)
		print("port 25: connected?\n");
	else
		print("port 25: %r\n");
	exits(nil);
}
