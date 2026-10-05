#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #I: wasm32's network, /net/tcp over WebSockets (docs/architecture.md,
 * D2).  A browser opens no TCP connections: a connection is a WebSocket
 * to the machine's webterm (sys/src/cmd/webterm.c), which carries a
 * service's bytes unchanged - GET /17019 rcpu, /567 auth - as drawterm's
 * wsock.c does.  The address's host is not used: the WebSocket server is
 * the machine.  The page owns the WebSockets (a Worker waits in
 * Atomics.wait and would not hear them): what comes in it writes into
 * the conversation's ring here, what goes out goes to it as a message.
 * A conversation's WebSocket is the page's by (n, gen): a new gen each
 * connect, so an old connection's close, data or end touches no new one
 * whatever order the page gets the Workers' messages in.  connect,
 * hangup and writes go under the conversation's QLock, a write with the
 * gen of the connection open when it was made.  The page empties the
 * ring when it opens a gen, and only the page writes it, all on its one
 * thread; connect asks for that only when no reader of an older gen is
 * in the ring (readers), and a reader leaves when the gen changes or
 * the connection goes, within a second.
 *
 *	/net/tcp/clone, /net/tcp/n/{ctl,data,local,remote,status}
 *	ctl: connect host!port, hangup
 *	/net/cs: a connection server, as ndb/cs answers - net or tcp, a
 *	host as it is, a service by the names below or its number
 */
enum
{
	Nconv	= 64,
	Nring	= 64*1024,
	Sendhigh	= 1024*1024,	/* what the page may hold for a conversation before its writer waits */
	Sendchunk	= 64*1024,	/* a write goes to the page in pieces of at most this, each reserved first */

	Qtop	= 1,
	Qcs,
	Qtcp,
	Qclone,
	Qconv,
	Qctl,
	Qdata,
	Qlocal,
	Qremote,
	Qstatus,
};

#define	QID(c, t)	(((c)+1)<<8 | (t))
#define	QTYPE(q)	((ulong)(q).path & 0xFF)
#define	QCONV(q)	(((ulong)(q).path >> 8) - 1)

typedef struct Ring Ring;
struct Ring
{
	ulong	r;		/* ours: read; modulo 2^32, Nring a power of 2 (the index masked) */
	ulong	w;		/* the page's: bytes written */
	long	closed;		/* the page's: 1 the WebSocket closed, 2 it failed, 3 too much came */
	long	sendq;		/* bytes of ours the page holds or will: we add before we send (sendreserve), it takes off what it has let go */
	uchar	b[Nring];
};

typedef struct Conv Conv;
struct Conv
{
	QLock;
	int	ref;		/* open channels */
	int	used;		/* /net/tcp/n is there: set when all of it is ready (newconv) */
	int	taken;		/* newconv is making it */
	ulong	gen;		/* which connection of n this is: the page's WebSocket goes with (n, gen); one more each connect */
	int	readers;	/* procs in wsread's ring of this gen (convlock) */
	long	st;		/* the page's: 0 opening, 1 open, -1 it could not */
	char	raddr[64];
	Ring	*in;
	int	open;		/* a WebSocket, the page's */
};

static	Conv	convs[Nconv];
static	Lock	convlock;

static char *names[] = {
	[Qctl]		"ctl",
	[Qdata]		"data",
	[Qlocal]	"local",
	[Qremote]	"remote",
	[Qstatus]	"status",
};

/* port names wasm32 knows without a cs: /lib/ndb/common's */
static struct {
	char	*name;
	char	*port;
} services[] = {
	"rcpu",		"17019",
	"ticket",	"567",
	"exportfs",	"17007",
	"secstore",	"5356",
	"signup",	"17040",	/* Plan2001's accounts (docs/webauthn.md) */
	"passkey",	"17041",
	"dist",		"17050",	/* the wasm32 distribution (aux/distd: Plan2001's /boot/install) */
	"rcpuws",	"/rcpu",	/* webterm's rcpu session for an app's origin: GET /rcpu (aux/wsrcpu, D7) */
};

static int
wsgen(Chan *c, char*, Dirtab*, int, int s, Dir *dp)
{
	Qid q;
	int n, t;

	t = QTYPE(c->qid);
	if(s == DEVDOTDOT){
		switch(t){
		case Qtop:
		case Qtcp:
			mkqid(&q, Qtop, 0, QTDIR);
			devdir(c, q, "#I", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		mkqid(&q, Qtcp, 0, QTDIR);
		devdir(c, q, "tcp", 0, eve, DMDIR|0555, dp);
		return 1;
	}
	/* a file gives its directory's entries: devstat looks for it among them */
	switch(t){
	case Qtop:
	case Qcs:
		if(s == 0){
			mkqid(&q, Qtcp, 0, QTDIR);
			devdir(c, q, "tcp", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(s > 1)
			return -1;
		mkqid(&q, Qcs, 0, QTFILE);
		devdir(c, q, "cs", 0, eve, 0666, dp);
		return 1;
	case Qtcp:
	case Qclone:
		if(s == 0){
			mkqid(&q, Qclone, 0, QTFILE);
			devdir(c, q, "clone", 0, eve, 0666, dp);
			return 1;
		}
		n = s-1;
		if(n >= Nconv)
			return -1;
		if(!convs[n].used)
			return 0;
		/* devdir keeps the pointer: the name in up->genbuf, as 9front's */
		snprint(up->genbuf, sizeof up->genbuf, "%d", n);
		mkqid(&q, QID(n, Qconv), 0, QTDIR);
		devdir(c, q, up->genbuf, 0, eve, DMDIR|0555, dp);
		return 1;
	default:
		n = QCONV(c->qid);
		if(s >= Qstatus-Qctl+1)
			return -1;
		mkqid(&q, QID(n, Qctl+s), 0, QTFILE);
		devdir(c, q, names[Qctl+s], 0, eve, 0666, dp);
		return 1;
	}
}

static Chan*
wsattach(char *spec)
{
	Chan *c;

	c = devattach('I', spec);
	mkqid(&c->qid, Qtop, 0, QTDIR);
	return c;
}

static Walkqid*
wswalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, wsgen);
}

static int
wsstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, nil, 0, wsgen);
}

/*
 * a conversation: taken under the lock, made ready outside it, and only
 * then there (used) for another proc to find in /net/tcp
 */
static Conv*
newconv(void)
{
	Conv *cv;
	int i;

	lock(&convlock);
	for(i = 0; i < Nconv; i++){
		cv = &convs[i];
		if(!cv->used && !cv->taken && cv->ref == 0)
			break;
	}
	if(i == Nconv){
		unlock(&convlock);
		error(Enodev);
	}
	cv->taken = 1;
	unlock(&convlock);

	if(cv->in == nil && (cv->in = mallocz(sizeof(Ring), 1)) == nil){
		lock(&convlock);
		cv->taken = 0;
		unlock(&convlock);
		error(Enomem);
	}
	cv->st = 0;
	cv->open = 0;
	cv->raddr[0] = 0;
	cv->gen++;

	lock(&convlock);
	cv->taken = 0;
	cv->ref = 1;
	cv->used = 1;
	unlock(&convlock);
	return cv;
}

static Chan*
wsopen(Chan *c, int omode)
{
	Conv *cv;
	int t;

	t = QTYPE(c->qid);
	switch(t){
	case Qcs:
		c->aux = nil;
		break;
	case Qclone:
		cv = newconv();
		mkqid(&c->qid, QID(cv-convs, Qctl), 0, QTFILE);
		break;
	case Qctl:
	case Qdata:
	case Qlocal:
	case Qremote:
	case Qstatus:
		cv = &convs[QCONV(c->qid)];
		lock(&convlock);
		if(!cv->used){
			unlock(&convlock);
			error(Ehungup);
		}
		cv->ref++;
		unlock(&convlock);
		break;
	default:
		if(omode != OREAD)
			error(Eperm);
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
wsclose(Chan *c)
{
	Conv *cv;
	int t;

	t = QTYPE(c->qid);
	if(t == Qcs && (c->flag & COPEN)){
		free(c->aux);
		c->aux = nil;
	}
	if((c->flag & COPEN) == 0 || t < Qctl)
		return;
	cv = &convs[QCONV(c->qid)];
	lock(&convlock);
	if(--cv->ref == 0){
		if(cv->open)
			platnetclose(cv-convs, cv->gen);
		cv->open = 0;
		cv->used = 0;
	}
	unlock(&convlock);
}

/*
 * a note interrupts a wait here as sleep's does: notepending taken,
 * Eintr - left set, every wait after would end at once (a mount's
 * reader on this conversation flushed its rpc again and again)
 */
static void
netnote(void)
{
	if(up->notepending){
		up->notepending = 0;
		interrupted();
	}
}

/*
 * wait for the word to be other than v, a second at a time: a note
 * interrupts; secs > 0 a limit, a deadline by the clock (a count that
 * stopped at 0 waited for ever: the review's)
 */
static int
netwait(long *w, long v, int secs)
{
	ulong end;

	end = seconds() + secs;
	while(*w == v){
		netnote();
		if(secs > 0 && seconds() >= end)
			return -1;
		platwait(w, v, 1000);
	}
	return 0;
}

/*
 * Backpressure to the page: a piece is reserved in sendq before it is sent
 * - an atomic add, so no message is on its way the count does not have
 * (the review's: a count the page kept after the fact let a fast writer
 * queue without limit) - and the writer waits while the reservation would
 * take sendq past Sendhigh, as at a full pipe; with sendq 0 a piece always
 * goes, so the page holds at most Sendhigh+Sendchunk.  The page takes off
 * what it no longer holds: sent (a plain WebSocket) or acknowledged
 * (webterm's session).  Not under the conversation's QLock: a hangup does
 * not wait for it; a note interrupts.
 */
static void
sendreserve(Conv *cv, Ring *r, ulong g, long n)
{
	long v;

	for(;;){
		v = r->sendq;
		if(v > 0 && v + n > Sendhigh){
			if(!cv->open || cv->gen != g || r->closed)
				error(Ehungup);
			netnote();
			platwait(&r->sendq, v, 1000);
			continue;
		}
		if(cmpswap(&r->sendq, v, v + n))
			return;
	}
}

/* a reservation not sent after all */
static void
sendrelease(Ring *r, long n)
{
	long v;

	do
		v = r->sendq;
	while(!cmpswap(&r->sendq, v, v > n ? v - n : 0));
	platwake(&r->sendq, 1);
}

/* a service's port: a name /lib/ndb/common gives, or its number, or a path (/rcpu: the name's); nil if none */
static char*
service(char *p)
{
	int i;

	if(p[0] == '/' && strchr(p+1, '/') == nil && strchr(p, '!') == nil && strlen(p) < 24)
		return p;

	for(i = 0; i < nelem(services); i++)
		if(strcmp(p, services[i].name) == 0)
			return services[i].port;
	for(i = 0; p[i] != 0; i++)
		if(p[i] < '0' || p[i] > '9')
			return nil;
	return i > 0 ? p : nil;
}

/* cs: net!host!service or tcp!host!service, the clone file and the address */
static char*
csquery(char *q)
{
	char *f[3], *p;
	int n;

	n = getfields(q, f, nelem(f), 0, "!");
	if(n != 3 || strcmp(f[0], "net") != 0 && strcmp(f[0], "tcp") != 0)
		error("cs: no translation (only tcp on wasm32)");
	if((p = service(f[2])) == nil)
		error("cs: no such service");
	n = strlen(f[1]) + strlen(p) + 32;
	q = malloc(n);
	if(q == nil)
		error(Enomem);
	snprint(q, n, "/net/tcp/clone %s!%s", f[1], p);
	return q;
}

/* called with cv qlocked */
static void
connect(Conv *cv, char *addr)
{
	char path[32], *p;

	if(cv->open)
		error("already connected");
	if((p = strrchr(addr, '!')) == nil)
		error("bad address: host!port");
	if((p = service(p+1)) == nil)
		error("bad port");
	snprint(path, sizeof path, "%s%s", p[0] == '/' ? "" : "/", p);
	/* the page empties the ring when it opens: no reader of an older gen in it then */
	for(;;){
		lock(&convlock);
		if(cv->readers == 0)
			break;
		unlock(&convlock);
		tsleep(&up->sleep, return0, nil, 20);	/* a note: sleep's Eintr */
	}
	cv->gen++;
	cv->st = 0;
	cv->open = 1;
	unlock(&convlock);
	strecpy(cv->raddr, cv->raddr+sizeof cv->raddr, addr);
	platnetopen(cv-convs, cv->gen, path, cv->in, &cv->st);
	/* as a TCP connect gives up: the browser may hold a WebSocket back a long time after failures */
	if(waserror()){	/* a note */
		platnetclose(cv-convs, cv->gen);
		cv->open = 0;
		nexterror();
	}
	if(netwait(&cv->st, 0, 60) < 0){
		platnetclose(cv-convs, cv->gen);
		cv->open = 0;
		error("connection timed out");
	}
	poperror();
	if(cv->st < 0){
		cv->open = 0;
		error("connection refused");
	}
}

static long
wsread(Chan *c, void *a, long n, vlong off)
{
	Conv *cv;
	Ring *r;
	char buf[128];
	ulong m, i, w, g;

	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, nil, 0, wsgen);
	if(QTYPE(c->qid) == Qcs){
		if(c->aux == nil)
			return 0;
		return readstr(off, a, n, c->aux);
	}
	cv = &convs[QCONV(c->qid)];
	switch(QTYPE(c->qid)){
	case Qctl:
		snprint(buf, sizeof buf, "%ld", (long)(cv-convs));
		return readstr(off, a, n, buf);
	case Qlocal:
		return readstr(off, a, n, "::!0\n");
	case Qremote:
		snprint(buf, sizeof buf, "%s\n", cv->raddr);
		return readstr(off, a, n, buf);
	case Qstatus:
		snprint(buf, sizeof buf, "%s\n", !cv->open || cv->st < 0 ? "Closed" : cv->st == 0 ? "Syn_sent" :
			cv->in->closed ? "Closed" : "Established");
		return readstr(off, a, n, buf);
	case Qdata:
		/*
		 * the ring is the page's to empty until the WebSocket is open;
		 * in it as a reader of this gen until the gen changes or the
		 * connection goes (connect waits for no readers)
		 */
		lock(&convlock);
		if(!cv->open || cv->st != 1){
			unlock(&convlock);
			return 0;
		}
		g = cv->gen;
		cv->readers++;
		unlock(&convlock);
		if(waserror()){
			lock(&convlock);
			cv->readers--;
			unlock(&convlock);
			nexterror();
		}
		r = cv->in;
		m = 0;
		for(;;){
			w = r->w;
			if(r->r != w)
				break;
			if(r->closed || !cv->open || cv->gen != g)
				goto out;
			netnote();
			platwait((long*)&r->w, w, 1000);
		}
		m = w - r->r;
		if(m > n)
			m = n;
		for(i = 0; i < m; i++)
			((uchar*)a)[i] = r->b[(r->r + i) & (Nring-1)];
		coherence();
		r->r += m;
	out:
		poperror();
		lock(&convlock);
		cv->readers--;
		unlock(&convlock);
		return m;
	}
	error(Egreg);
	return 0;
}

static long
wswrite(Chan *c, void *a, long n, vlong)
{
	Conv *cv;
	Cmdbuf *cb;
	Ring *r;
	ulong g;
	long done, m;

	if(QTYPE(c->qid) == Qcs){
		char q[128];

		if(n >= sizeof q)
			error(Etoobig);
		memmove(q, a, n);
		q[n] = 0;
		free(c->aux);
		c->aux = nil;
		c->aux = csquery(q);
		return n;
	}
	cv = &convs[QCONV(c->qid)];
	switch(QTYPE(c->qid)){
	case Qctl:
		cb = parsecmd(a, n);
		if(waserror()){
			free(cb);
			nexterror();
		}
		qlock(cv);
		if(waserror()){
			qunlock(cv);
			nexterror();
		}
		if(cb->nf == 2 && strcmp(cb->f[0], "connect") == 0)
			connect(cv, cb->f[1]);
		else if(cb->nf >= 1 && strcmp(cb->f[0], "hangup") == 0){
			if(cv->open)
				platnetclose(cv-convs, cv->gen);
			cv->open = 0;
		}else
			error(Ebadctl);
		poperror();
		qunlock(cv);
		poperror();
		free(cb);
		return n;
	case Qdata:
		/* in pieces, each reserved in the page first; with the gen of the connection open now */
		r = cv->in;
		g = cv->gen;
		if(r == nil)
			error(Ehungup);
		for(done = 0; done < n; done += m){
			m = n - done;
			if(m > Sendchunk)
				m = Sendchunk;
			sendreserve(cv, r, g, m);
			qlock(cv);
			if(!cv->open || cv->st != 1 || r->closed || cv->gen != g){
				qunlock(cv);
				sendrelease(r, m);
				error(Ehungup);
			}
			platnetsend(cv-convs, cv->gen, (uchar*)a + done, m);
			qunlock(cv);
		}
		return n;
	}
	error(Eperm);
	return 0;
}

Dev wsnetdevtab = {
	'I',
	"wsnet",

	devreset,
	devinit,
	devshutdown,
	wsattach,
	wswalk,
	wsstat,
	wsopen,
	devcreate,
	wsclose,
	wsread,
	devbread,
	wswrite,
	devbwrite,
	devremove,
	devwstat,
};
