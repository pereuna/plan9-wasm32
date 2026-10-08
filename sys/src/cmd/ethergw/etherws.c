/*
 * etherws - a WebSocket to ethernetd (docs/network.md): GET /auth from the
 * auth origin's page (-A: its passkeys), GET /ether from the machine's
 * page (-E: its Ethernet card's frames).  One connection, on fd 0 and 1:
 * rc-httpd's request (-r: $request and $reqlines, as webterm -r takes
 * them), else read here (aux/listen1, the tests).  The page's Origin must
 * be the path's; a request without one is not a page's, and is refused.
 *
 * ethernetd is reached through /srv: a pipe of this connection's is
 * posted (/srv/SRVNAME.PID, mode 0660: the host owner's, ORCLOSE), and
 * "conn SRVNAME.PID KIND ADDR" written to /srv/SRVNAME; on the pipe a
 * message is a write, its first byte T (text) or B (binary), as the
 * WebSocket's; X alone from ethernetd ends the connection.  What comes from the page: masked frames with FIN (no
 * fragments), a text at most 8K, a binary message an Ethernet frame (14
 * to 1514 bytes); anything else ends the connection.  ADDR is $remote
 * (the caller's: the browser's address), else $net/remote's, else -.
 *
 *	aux/etherws [-r] -A authorigin -E machineorigin [-s srvname]
 */
#include <u.h>
#include <libc.h>
#include <mp.h>
#include <libsec.h>

enum {
	Maxhdr	= 8192,
	Maxtext	= 8192,
	Minframe	= 14,
	Maxframe	= 1514,
};

static char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static char *authorigin, *machineorigin;
static char *srvname = "ethernetd";
static int fromhttpd;

static void
reply(char *status)
{
	fprint(1, "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
	exits(status);
}

/* the request up to its blank line, as a string */
static char*
readhdr(void)
{
	static char buf[Maxhdr+1];
	int n;

	for(n = 0; n < Maxhdr; n++){
		if(read(0, buf+n, 1) != 1)
			exits("eof");
		buf[n+1] = 0;
		if(n >= 3 && strcmp(buf+n-3, "\r\n\r\n") == 0)
			return buf;
	}
	reply("431 Request Header Fields Too Large");
	return nil;
}

/* the request rc-httpd has read (-r): its request line and header lines, one to a line */
static char*
httpdhdr(void)
{
	char *req, *lines, *p, *e, *buf;
	int n;

	req = getenv("request");
	lines = getenv("reqlines");
	if(req == nil || lines == nil)
		reply("400 Bad Request");
	buf = malloc(strlen(req) + 2*strlen(lines) + 8);
	if(buf == nil)
		reply("500 Internal Server Error");
	n = sprint(buf, "%s\r\n", req);
	for(p = lines; *p != 0; p = e+1){
		if((e = strchr(p, '\n')) == nil)
			e = p + strlen(p);
		if(e > p){
			memmove(buf+n, p, e-p);
			n += e-p;
			strcpy(buf+n, "\r\n");
			n += 2;
		}
		if(*e == 0)
			break;
	}
	strcpy(buf+n, "\r\n");
	if(n + 2 > Maxhdr)
		reply("431 Request Header Fields Too Large");
	return buf;
}

/* the value of header name, or nil */
static char*
header(char *hdr, char *name)
{
	static char val[Maxhdr];
	char *p, *e;
	int n;

	n = strlen(name);
	for(p = hdr; (p = strstr(p, "\r\n")) != nil; ){
		p += 2;
		if(cistrncmp(p, name, n) == 0 && p[n] == ':'){
			p += n+1;
			while(*p == ' ' || *p == '\t')
				p++;
			if((e = strstr(p, "\r\n")) == nil || e-p >= sizeof val)
				return nil;
			memmove(val, p, e-p);
			val[e-p] = 0;
			return val;
		}
	}
	return nil;
}

static void
wsaccept(char *hdr)
{
	char *key, *v, buf[128], accept[64];
	uchar digest[SHA1dlen];

	if((v = header(hdr, "Sec-WebSocket-Version")) == nil || strcmp(v, "13") != 0)
		reply("426 Upgrade Required");
	if((key = header(hdr, "Sec-WebSocket-Key")) == nil || strlen(key) > 64)
		reply("400 Bad Request");
	snprint(buf, sizeof buf, "%s%s", key, guid);
	sha1((uchar*)buf, strlen(buf), digest, nil);
	enc64(accept, sizeof accept, digest, SHA1dlen);
	fprint(1, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
}

/* one frame to the browser: unmasked, FIN (a write: the two procs' frames do not mix) */
static int
sendframe(int op, uchar *data, long n)
{
	uchar *b;
	int h, r;

	if(n > Maxtext || (b = malloc(4+n)) == nil)
		return -1;
	b[0] = 0x80 | op;
	if(n < 126){
		b[1] = n;
		h = 2;
	}else{
		b[1] = 126;
		b[2] = n>>8;
		b[3] = n;
		h = 4;
	}
	memmove(b+h, data, n);
	r = write(1, b, h+n) == h+n ? 0 : -1;
	free(b);
	return r;
}

/* one frame from the browser: masked, FIN, at most Maxtext; its op, -1 at an end */
static int
readframe(uchar *p, long *np)
{
	uchar h[8], mask[4];
	long n, i;
	int op;

	if(readn(0, h, 2) != 2)
		return -1;
	op = h[0] & 0x0F;
	if((h[0] & 0x80) == 0 || (h[0] & 0x70) != 0 || (h[1] & 0x80) == 0)
		return -1;	/* a fragment, an extension's bits, not masked */
	n = h[1] & 0x7F;
	if(n == 126){
		if(readn(0, h+2, 2) != 2)
			return -1;
		n = h[2]<<8 | h[3];
	}else if(n == 127)
		return -1;
	if(n > Maxtext || readn(0, mask, 4) != 4 || readn(0, p, n) != n)
		return -1;
	for(i = 0; i < n; i++)
		p[i] ^= mask[i%4];
	*np = n;
	return op;
}

/* the procs' buffers, not on their stacks */
static uchar downbuf[1+Maxtext], upbuf[1+Maxtext];

/* ethernetd to the browser; its end (or a message of one byte, X: ethernetd's) the WebSocket's */
static void
downstream(int fd)
{
	uchar *buf = downbuf;
	int n;

	while((n = read(fd, buf, sizeof downbuf)) > 1)
		if(sendframe(buf[0] == 'T' ? 1 : 2, buf+1, n-1) < 0)
			break;
	sendframe(8, nil, 0);
}

/* the browser to ethernetd */
static void
upstream(int fd)
{
	uchar *buf = upbuf;
	long n;
	int op;

	while((op = readframe(buf+1, &n)) >= 0){
		switch(op){
		case 1:		/* text */
			buf[0] = 'T';
			break;
		case 2:		/* binary: a frame */
			if(n < Minframe || n > Maxframe)
				return;
			buf[0] = 'B';
			break;
		case 9:		/* ping */
			sendframe(10, buf+1, n);
			continue;
		case 10:	/* pong */
			continue;
		default:	/* close, a continuation, the rest */
			return;
		}
		if(write(fd, buf, 1+n) != 1+n)
			return;
	}
}

/* the browser's address: $remote (the caller's), else the connection's ($net/remote: aux/listen's) */
static char*
remote(void)
{
	static char buf[128];
	char *a, *p;
	int fd, n;

	if((a = getenv("remote")) == nil && (p = getenv("net")) != nil){
		a = smprint("%s/remote", p);
		n = -1;
		if((fd = open(a, OREAD)) >= 0){
			n = read(fd, buf, sizeof buf - 1);
			close(fd);
		}
		free(a);
		a = nil;
		if(n > 0){
			buf[n] = 0;
			a = buf;
		}
	}
	if(a == nil)
		return "-";
	a[strcspn(a, " \t\n")] = 0;
	return *a ? a : "-";
}

/* this connection's pipe to ethernetd */
static int
dialethernetd(char *kind)
{
	char name[64], path[80], *addr;
	int p[2], fd, ctl;

	if(pipe(p) < 0)
		return -1;
	snprint(name, sizeof name, "%s.%d", srvname, getpid());
	snprint(path, sizeof path, "/srv/%s", name);
	if((fd = create(path, OWRITE|ORCLOSE, 0660)) < 0)
		return -1;
	if(fprint(fd, "%d", p[0]) < 0)
		return -1;
	close(p[0]);
	snprint(path, sizeof path, "/srv/%s", srvname);
	if((ctl = open(path, OWRITE)) < 0)
		return -1;
	addr = remote();
	if(fprint(ctl, "conn %s %s %s", name, kind, addr) < 0)
		return -1;
	close(ctl);
	/* fd, the posted pipe, stays open: closed at the end, it is removed */
	return p[1];
}

static void
usage(void)
{
	fprint(2, "usage: aux/etherws [-r] -A authorigin -E machineorigin [-s srvname]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *hdr, *path, *e, *up, *o, *kind, *want;
	int fd, pid, ppid;

	ARGBEGIN{
	case 'r':
		fromhttpd = 1;
		break;
	case 'A':
		authorigin = EARGF(usage());
		break;
	case 'E':
		machineorigin = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0 || authorigin == nil || machineorigin == nil)
		usage();

	hdr = fromhttpd ? httpdhdr() : readhdr();
	if(strncmp(hdr, "GET /", 5) != 0)
		reply("405 Method Not Allowed");
	path = hdr+4;
	if((e = strpbrk(path, " ?")) == nil)
		reply("400 Bad Request");
	*e = 0;
	path = strdup(path);
	*e = ' ';
	if(strcmp(path, "/auth") == 0){
		kind = "auth";
		want = authorigin;
	}else if(strcmp(path, "/ether") == 0){
		kind = "ether";
		want = machineorigin;
	}else
		reply("404 Not Found");
	up = header(hdr, "Upgrade");
	if(up == nil || cistrcmp(up, "websocket") != 0)
		reply("426 Upgrade Required");
	if((o = header(hdr, "Origin")) == nil || strcmp(o, want) != 0)
		reply("403 Forbidden");
	if((fd = dialethernetd(kind)) < 0)
		reply("503 Service Unavailable");
	wsaccept(hdr);
	/* two processes, nothing shared between them but the connection; either's end is both's */
	ppid = getpid();
	switch(pid = rfork(RFPROC|RFFDG)){
	case -1:
		exits("rfork");
	case 0:
		downstream(fd);
		postnote(PNPROC, ppid, "kill");
		exits(nil);
	}
	upstream(fd);
	postnote(PNPROC, pid, "kill");
	exits(nil);
}
