/*
 * ethernetd - the wasm32 machines' network on a server (docs/network.md):
 * a browser's machine (its Ethernet card, etherwasm32.c) reaches the
 * internet through this machine, one that has shown a passkey.  aux/etherws
 * brings each WebSocket here (one per connection, as none, from rc-httpd);
 * this runs once, as the host owner, from cpustart.
 *
 * A connection's first message (etherws's ctl write to /srv/ethernetd) is
 *	conn SRVNAME KIND ADDR
 * SRVNAME a pipe in /srv (etherws's, mode 0660), KIND auth or ether, ADDR
 * the browser's address (for the log).  On the pipe a message is a write:
 * its first byte T (a WebSocket text message) or B (a binary one, an
 * Ethernet frame), then its bytes; X alone, from here, ends the connection.
 *
 * auth (the auth origin's page, -o; its passkeys' rp -r):
 *	<- challenge C			32 random bytes, base64url
 *	-> enroll INVITE ID SPKI AUTH CLIENT	a new passkey (create), with an invitation
 *	-> login ID AUTH CLIENT SIG	a passkey's assertion (get)
 *	<- ok TICKET | error WHY
 * An invitation is a line of DIR/invites, CODE NAME: used once, the line
 * removed; without the file there is no enrolling.  A passkey is DIR/cred/ID
 * (name, pubkey, count, created).  The passkey must have verified its user
 * (UV) and been present (UP), for this rp, from that origin, this challenge;
 * an assertion's signature (ES256) by the key kept, its counter above the
 * last.  A ticket is 32 random bytes in this process's memory, an hour's.
 *
 * ether (the machine's page):
 *	-> ticket T
 *	<- ok | error WHY
 *	then frames both ways, and from time to time
 *	<- ticket T2	the ticket renewed (the old one good until it ends)
 * One connection a name: a new one takes the old one's place (a page
 * reloaded, a connection lost).  The machine is a host of NET (-n,
 * 100.64.0.0/24: not the 10.* or 192.168.* this server's own services
 * trust), the gateway its .1, which answers its ARP, DHCP and ping; the rest
 * goes into this machine's IP stack through a pkt interface, and out with
 * its address (source translation, the route's t: 9front's NAT).  What goes
 * out: its own address only, TCP to ports 80, 443 and 9418, DNS (UDP and
 * TCP 53) to the resolvers (-D), ICMP echo; not to this machine, nor the
 * private, shared, link-local (the cloud's metadata), multicast or reserved
 * networks; no fragments, no IPv6.  Each machine has -R bytes a second
 * each way (1 MB), 20 new TCP connections a second, 2 GB a day a name; at
 * most -m at once (8); -i idle seconds end it (15 minutes).  The log (syslog ether, or -l: standard error) has the
 * passkeys, the connections, their bytes and each new TCP connection's
 * destination.
 *
 *	aux/ethernetd -r rpid -o origin [-d dir] [-s srvname] [-n net] [-g gate ifc]
 *		[-D resolver]... [-m max] [-i idle] [-R rate] [-l] [-v]
 * (-v: each connection in the log too)
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <mp.h>
#include <libsec.h>
#include <thread.h>
#include <ip.h>

enum {
	Stack	= 64*1024,
	Nchal	= 32,
	Nticket	= 32,
	Maxmsg	= 8*1024,	/* a message: a text's longest */
	Ttl	= 60*60,	/* a ticket's seconds */
	Renew	= 30*60,	/* a ticket in use is renewed after */
	Authtime	= 120,	/* seconds for a page to answer */
	Burst	= 256*1024,
	Synrate	= 20,
	Nout	= 256,	/* frames waiting for a connection: more are dropped */
	Nlease	= 256,
	Etherhdr	= 14,
	Maxframe	= 1514,
};

typedef struct Bucket Bucket;
typedef struct Conn Conn;
typedef struct Day Day;
typedef struct Msg Msg;
typedef struct Net Net;
typedef struct Ticket Ticket;

struct Bucket
{
	vlong	t;	/* ms */
	double	n;
};

struct Msg
{
	int	n;
	uchar	b[1];	/* T or B, then its bytes */
};

struct Ticket
{
	char	s[64];	/* base64url */
	char	prev[64];	/* the one it renewed, until it ends */
	char	*name;
	long	expires;
	long	prevexpires;
	long	issued;
	Ticket	*next;
};

struct Conn
{
	int	fd;
	char	*addr;
	int	kind;
	Channel	*out;	/* Msg* to the writer; nil ends it */
	Ticket	*t;
	char	*name;
	int	lease;	/* its host on NET, 0 none */
	uchar	mac[6];
	int	hasmac;
	Bucket	up, down, syn;
	long	start;
	long	last;	/* a frame from it */
	vlong	bytes[2];	/* up, down */
	int	dead;	/* to be ended */
	int	ended;	/* its end sent to etherws */
	Conn	*next;	/* conns */
};

struct Day
{
	char	*name;
	long	day;
	vlong	bytes;
	Day	*next;
};

struct Net
{
	ulong	a, m;
};

enum { Kauth, Kether };

static char *rp, *authorigin;
static char *dir = "/adm/ether";
static char *srvname = "ethernetd";
static int logerr, verbose;
static int maxmach = 8;
static int idle = 15*60;	/* -i: seconds without a frame that end a machine's connection */
static double rate = 1000*1000;
static vlong daycap = 2000LL*1000*1000;
static ulong net = 0x64400000, mask = 0xffffff00;	/* 100.64.0.0/24 */
static uchar gwmac[6] = { 0x02, 0x00, 0x64, 0x00, 0x00, 0x01 };
static ulong resolvers[8];
static int nresolvers;
static ulong self[32];	/* this machine's addresses */
static int nself;
static char *gate, *gateifc;
static int netfd = -1;

static QLock lk;	/* the tickets, leases and days */
static Ticket *tickets;
static Conn *leases[Nlease];
static Conn *conns;	/* all of them, for the timer */
static Day *days;
static QLock invlk, credlk;	/* the invitations and the passkeys' files */

static Net blocked[] = {
	0x00000000, 0xff000000,	/* this network */
	0x0a000000, 0xff000000,	/* private */
	0x64400000, 0xffc00000,	/* shared (carrier NAT; the machines') */
	0x7f000000, 0xff000000,	/* loopback */
	0xa9fe0000, 0xffff0000,	/* link-local: the cloud's metadata */
	0xac100000, 0xfff00000,	/* private */
	0xc0000000, 0xffffff00,	/* IETF */
	0xc0a80000, 0xffff0000,	/* private */
	0xc6120000, 0xfffe0000,	/* benchmarking */
	0xe0000000, 0xf0000000,	/* multicast */
	0xf0000000, 0xf0000000,	/* reserved, broadcast */
};

static void
elog(char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	if(logerr)
		fprint(2, "ethernetd: %s\n", buf);
	else
		syslog(0, "ether", "%s", buf);
}

static int
take(Bucket *b, double r, double burst, double n)
{
	vlong now;

	now = nsec()/1000000;
	if(b->t == 0)
		b->n = burst;
	else
		b->n += (now - b->t) * r / 1000;
	if(b->n > burst)
		b->n = burst;
	b->t = now;
	if(b->n < n)
		return 0;
	b->n -= n;
	return 1;
}

/* base64url, without padding */
static int
unb64u(char *s, uchar *p, int n)
{
	char *t, *u;
	int m;

	m = strlen(s);
	if(m == 0 || strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != m)
		return -1;
	t = malloc(m + 4);
	if(t == nil)
		return -1;
	strcpy(t, s);
	for(u = t; *u; u++)
		if(*u == '-')
			*u = '+';
		else if(*u == '_')
			*u = '/';
	while(m % 4)
		t[m++] = '=';
	t[m] = 0;
	m = dec64(p, n, t, m);
	free(t);
	return m;
}

static void
b64u(char *s, int ns, uchar *p, int n)
{
	char *t;

	enc64(s, ns, p, n);
	for(t = s; *t; t++)
		if(*t == '+')
			*t = '-';
		else if(*t == '/')
			*t = '_';
		else if(*t == '='){
			*t = 0;
			break;
		}
}

static int
isb64u(char *s, int min, int max)
{
	int n;

	n = strlen(s);
	return n >= min && n <= max && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == n;
}

/* clientDataJSON's "key":"value" (the browsers' serialization: no spaces; values without escapes we take) */
static char*
jsonstr(char *json, char *key)
{
	char *k, *p, *e;

	k = smprint("\"%s\":\"", key);
	p = strstr(json, k);
	if(p == nil){
		free(k);
		return nil;
	}
	p += strlen(k);
	free(k);
	if((e = strchr(p, '"')) == nil)
		return nil;
	return smprint("%.*s", (int)(e-p), p);
}

/* the page's word for what was asked, from where, for this challenge; nil if so */
static char*
clientok(char *client, char *type, char *chal)
{
	char *s;
	int ok;

	if((s = jsonstr(client, "type")) == nil)
		return "no type";
	ok = strcmp(s, type) == 0;
	free(s);
	if(!ok)
		return "not what was asked";
	if((s = jsonstr(client, "challenge")) == nil)
		return "no challenge";
	ok = strcmp(s, chal) == 0;
	free(s);
	if(!ok)
		return "not this challenge";
	if(strstr(client, "\"crossOrigin\":true") != nil)
		return "cross-origin";
	if((s = jsonstr(client, "origin")) == nil)
		return "no origin";
	ok = strcmp(s, authorigin) == 0;
	free(s);
	if(!ok)
		return "not this site's origin";
	return nil;
}

/* authenticatorData: rpIdHash[32] flags[1] counter[4]; the user present and verified */
static char*
authok(uchar *auth, int nauth)
{
	uchar h[SHA2_256dlen];

	if(nauth < 37)
		return "bad authenticator data";
	sha2_256((uchar*)rp, strlen(rp), h, nil);
	if(memcmp(auth, h, sizeof h) != 0)
		return "not this site's passkey";
	if((auth[32] & 0x01) == 0)
		return "no user present";
	if((auth[32] & 0x04) == 0)
		return "the user not verified";
	return nil;
}

static ulong
counter(uchar *auth)
{
	return (ulong)auth[33]<<24 | auth[34]<<16 | auth[35]<<8 | auth[36];
}

/* an SPKI's P-256 key (its last 65 bytes the point) */
static ECpub*
spkipub(char *spki, ECdomain *dom)
{
	uchar key[256];
	ECpub *pub;
	int n;

	n = unb64u(spki, key, sizeof key);
	if(n != 91 || key[26] != 4)
		return nil;
	ecdominit(dom, secp256r1);
	if((pub = ecdecodepub(dom, key+26, 65)) == nil){
		ecdomfree(dom);
		return nil;
	}
	return pub;
}

/* the assertion's signature, ES256, over authenticatorData and sha256(clientDataJSON) */
static int
verify(char *spki, uchar *auth, int nauth, uchar *client, int nclient, uchar *sig, int nsig)
{
	uchar h[SHA2_256dlen], d[SHA2_256dlen];
	ECdomain dom;
	ECpub *pub;
	DigestState *ds;
	char *e;

	if((pub = spkipub(spki, &dom)) == nil)
		return -1;
	sha2_256(client, nclient, h, nil);
	ds = sha2_256(auth, nauth, nil, nil);
	sha2_256(h, sizeof h, d, ds);
	e = X509ecdsaverifydigest(sig, nsig, d, sizeof d, &dom, pub);
	ecpubfree(pub);
	ecdomfree(&dom);
	return e == nil ? 0 : -1;
}

typedef struct Rec Rec;
struct Rec
{
	char	*name;
	char	*pubkey;
	ulong	count;
	char	*created;
};

static Rec*
readrec(char *path)
{
	Biobuf *b;
	char *l, *v;
	Rec *r;

	if((b = Bopen(path, OREAD)) == nil)
		return nil;
	r = mallocz(sizeof *r, 1);
	while((l = Brdstr(b, '\n', 1)) != nil){
		v = l + strcspn(l, " \t");
		if(*v != 0)
			*v++ = 0;
		v += strspn(v, " \t");
		if(strcmp(l, "name") == 0)
			r->name = strdup(v);
		else if(strcmp(l, "pubkey") == 0)
			r->pubkey = strdup(v);
		else if(strcmp(l, "count") == 0)
			r->count = strtoul(v, nil, 10);
		else if(strcmp(l, "created") == 0)
			r->created = strdup(v);
		free(l);
	}
	Bterm(b);
	if(r->name == nil || r->pubkey == nil){
		free(r);
		return nil;
	}
	return r;
}

static int
writerec(int fd, Rec *r)
{
	char *s;
	int n;

	s = smprint("name\t%s\npubkey\t%s\ncount\t%lud\ncreated\t%s\n", r->name, r->pubkey, r->count, r->created ? r->created : "");
	n = strlen(s);
	if(seek(fd, 0, 0) < 0 || write(fd, s, n) != n){
		free(s);
		return -1;
	}
	free(s);
	return 0;
}

static int
nameok(char *s)
{
	int n;

	n = strlen(s);
	return n >= 1 && n <= 28 && s[0] >= 'a' && s[0] <= 'z' && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789") == n;
}

/*
 * The invitation CODE's name, the line taken out of the file; nil (and
 * why) if there is none.  invlk held.
 */
static char*
invitation(char *code, char **why, int consume)
{
	char *path, *buf, *l, *e, *name, *t, *f[3], *rest;
	int fd, n, m, nf;
	Dir *d;

	*why = "no such invitation";
	if(!isb64u(code, 16, 128))
		return nil;
	path = smprint("%s/invites", dir);
	if((fd = open(path, OREAD)) < 0){
		*why = "no invitations";	/* without the file, no enrolling */
		free(path);
		return nil;
	}
	if((d = dirfstat(fd)) == nil || d->length > 1024*1024){
		free(d);
		close(fd);
		free(path);
		return nil;
	}
	n = d->length;
	free(d);
	buf = malloc(n+1);
	rest = malloc(n+2);
	if(buf == nil || rest == nil || readn(fd, buf, n) != n){
		free(buf);
		free(rest);
		close(fd);
		free(path);
		return nil;
	}
	close(fd);
	buf[n] = 0;
	name = nil;
	m = 0;
	for(l = buf; *l != 0; l = e){
		if((e = strchr(l, '\n')) != nil)
			*e++ = 0;
		else
			e = l + strlen(l);
		if(name == nil){
			t = strdup(l);
			nf = tokenize(t, f, nelem(f));
			if(nf == 2 && strlen(f[0]) == strlen(code) && tsmemcmp(f[0], code, strlen(code)) == 0 && nameok(f[1])){
				name = strdup(f[1]);
				free(t);
				continue;
			}
			free(t);
		}
		m += sprint(rest+m, "%s\n", l);
	}
	free(buf);
	/* used: the file without its line */
	if(name != nil && consume)
		if((fd = open(path, OWRITE|OTRUNC)) < 0 || write(fd, rest, m) != m || close(fd) < 0){
			*why = "can not use the invitation";
			free(name);
			name = nil;
		}
	free(rest);
	free(path);
	return name;
}

static char*
enroll(char **f, char *chal, char **namep)
{
	uchar auth[1024], client[4096];
	int nauth, nclient, fd, n;
	char *why, *e, *path, *name;
	ECdomain dom;
	ECpub *pub;
	Rec r;

	if(!isb64u(f[2], 16, 1024))
		return "bad passkey id";
	if((pub = spkipub(f[3], &dom)) == nil)
		return "not a P-256 key";
	ecpubfree(pub);
	ecdomfree(&dom);
	nauth = unb64u(f[4], auth, sizeof auth);
	nclient = unb64u(f[5], client, sizeof client - 1);
	if(nauth < 37 || nclient <= 0)
		return "bad passkey";
	client[nclient] = 0;
	if((e = clientok((char*)client, "webauthn.create", chal)) != nil)
		return e;
	if((e = authok(auth, nauth)) != nil)
		return e;

	qlock(&invlk);
	if((name = invitation(f[1], &why, 0)) == nil){
		qunlock(&invlk);
		return why;
	}
	path = smprint("%s/cred/%s", dir, f[2]);
	if((fd = create(path, OWRITE|OEXCL, 0600)) < 0){
		qunlock(&invlk);
		free(path);
		free(name);
		return "the passkey is known";
	}
	r.name = name;
	r.pubkey = f[3];
	r.count = counter(auth);
	r.created = smprint("%ld", time(0));
	n = writerec(fd, &r);
	if(close(fd) < 0)
		n = -1;
	free(name);
	if(n < 0 || (name = invitation(f[1], &why, 1)) == nil){
		remove(path);
		qunlock(&invlk);
		free(path);
		free(r.created);
		return "can not keep the passkey";
	}
	qunlock(&invlk);
	free(path);
	free(r.created);
	*namep = name;
	return nil;
}

static char*
login(char **f, char *chal, char **namep)
{
	uchar auth[1024], client[4096], sig[256];
	int nauth, nclient, nsig, fd;
	char *e, *path;
	ulong count;
	Rec *r;

	if(!isb64u(f[1], 16, 1024))
		return "no such passkey";
	nauth = unb64u(f[2], auth, sizeof auth);
	nclient = unb64u(f[3], client, sizeof client - 1);
	nsig = unb64u(f[4], sig, sizeof sig);
	if(nauth < 37 || nclient <= 0 || nsig <= 0)
		return "bad assertion";
	client[nclient] = 0;
	if((e = clientok((char*)client, "webauthn.get", chal)) != nil)
		return e;
	if((e = authok(auth, nauth)) != nil)
		return e;

	path = smprint("%s/cred/%s", dir, f[1]);
	qlock(&credlk);
	if((r = readrec(path)) == nil){
		qunlock(&credlk);
		free(path);
		return "no such passkey";
	}
	count = counter(auth);
	if((count != 0 || r->count != 0) && count <= r->count){
		qunlock(&credlk);
		free(path);
		return "counter not above the last: a copied passkey?";
	}
	if(verify(r->pubkey, auth, nauth, client, nclient, sig, nsig) < 0){
		qunlock(&credlk);
		free(path);
		return "bad signature";
	}
	r->count = count;
	if((fd = open(path, OWRITE|OTRUNC)) < 0 || writerec(fd, r) < 0){
		if(fd >= 0)
			close(fd);
		qunlock(&credlk);
		free(path);
		return "can not keep the counter";
	}
	close(fd);
	qunlock(&credlk);
	free(path);
	*namep = r->name;
	return nil;
}

static void
newticketstr(char *s, int n)
{
	uchar b[Nticket];

	genrandom(b, sizeof b);
	b64u(s, n, b, sizeof b);
}

/* lk held */
static Ticket*
newticket(char *name)
{
	Ticket *t;

	t = mallocz(sizeof *t, 1);
	newticketstr(t->s, sizeof t->s);
	t->name = strdup(name);
	t->issued = time(0);
	t->expires = t->issued + Ttl;
	t->next = tickets;
	tickets = t;
	return t;
}

/* the ticket s names, those ended gone; lk held */
static Ticket*
findticket(char *s)
{
	Ticket *t, **l;
	long now;
	int n;

	now = time(0);
	n = strlen(s);
	for(l = &tickets; (t = *l) != nil; ){
		if(t->expires <= now){
			*l = t->next;
			free(t->name);
			free(t);
			continue;
		}
		if(n == strlen(t->s) && tsmemcmp(s, t->s, n) == 0)
			return t;
		if(t->prevexpires > now && n == strlen(t->prev) && tsmemcmp(s, t->prev, n) == 0)
			return t;
		l = &t->next;
	}
	return nil;
}

/* n more bytes of name's today: 0 if past its day's; lk held */
static int
daycountl(char *name, vlong n)
{
	Day *d;
	long today;
	int ok;

	today = time(0) / (24*60*60);
	for(d = days; d != nil; d = d->next)
		if(strcmp(d->name, name) == 0)
			break;
	if(d == nil){
		d = mallocz(sizeof *d, 1);
		d->name = strdup(name);
		d->next = days;
		days = d;
	}
	if(d->day != today){
		d->day = today;
		d->bytes = 0;
	}
	ok = d->bytes + n <= daycap;
	if(ok)
		d->bytes += n;
	return ok;
}

static int
daycount(char *name, vlong n)
{
	int ok;

	qlock(&lk);
	ok = daycountl(name, n);
	qunlock(&lk);
	return ok;
}

static Msg*
mkmsg(int type, void *p, int n)
{
	Msg *m;

	m = malloc(sizeof *m + n);
	if(m == nil)
		return nil;
	m->b[0] = type;
	memmove(m->b+1, p, n);
	m->n = 1+n;
	return m;
}

static void
sendtext(Conn *c, char *fmt, ...)
{
	char buf[256];
	va_list arg;
	Msg *m;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	if((m = mkmsg('T', buf, strlen(buf))) != nil)
		sendp(c->out, m);
}

/* a frame to the machine: dropped if its queue is full */
static void
sendframe(Conn *c, uchar *f, int n)
{
	Msg *m;

	if((m = mkmsg('B', f, n)) != nil && nbsendp(c->out, m) == 0)
		free(m);
}

/* the end of the connection: etherws closes the WebSocket (a message of its type byte alone) */
static void
sendend(Conn *c)
{
	Msg *m;

	if((m = mkmsg('X', nil, 0)) != nil)
		sendp(c->out, m);
}

/* the same, lk held: not waiting for the writer (the timer tries again) */
static void
endl(Conn *c)
{
	Msg *m;

	if(c->ended || (m = mkmsg('X', nil, 0)) == nil)
		return;
	if(nbsendp(c->out, m))
		c->ended = 1;
	else
		free(m);
}

/*
 * What takes time, without notes (a proc's read is not interrupted): every
 * 5 seconds a connection that has not shown its passkey or ticket in
 * Authtime, or a machine idle past -i, is ended (etherws closes it, its
 * reader sees the end); a ticket in use past Renew is renewed (the page
 * keeps the new one; the old one good until it ends)
 */
static void
timerproc(void*)
{
	char buf[128];
	Conn *c;
	Ticket *t;
	Msg *m;
	long now;

	threadsetname("ethernetd timer");
	for(;;){
		sleep(5*1000);
		now = time(0);
		qlock(&lk);
		for(c = conns; c != nil; c = c->next){
			if(!c->dead && c->lease == 0 && now - c->start > Authtime)
				c->dead = 1;
			else if(!c->dead && c->lease != 0 && now - c->last > idle){
				elog("%s: %s idle", c->addr, c->name);
				c->dead = 1;
				if(leases[c->lease] == c)
					leases[c->lease] = nil;
			}else if(!c->dead && c->lease != 0 && (t = c->t) != nil && now - t->issued > Renew){
				strcpy(t->prev, t->s);
				t->prevexpires = t->expires;
				newticketstr(t->s, sizeof t->s);
				t->issued = now;
				t->expires = now + Ttl;
				snprint(buf, sizeof buf, "ticket %s", t->s);
				if((m = mkmsg('T', buf, strlen(buf))) != nil && nbsendp(c->out, m) == 0)
					free(m);
			}
			if(c->dead)
				endl(c);
		}
		qunlock(&lk);
	}
}

static void
writeproc(void *a)
{
	Conn *c;
	Msg *m;

	c = a;
	threadsetname("ethernetd writer");
	while((m = recvp(c->out)) != nil){
			if(write(c->fd, m->b, m->n) != m->n)
			c->dead = 1;
			free(m);
	}
	close(c->fd);
	while((m = nbrecvp(c->out)) != nil)
		free(m);
	chanfree(c->out);
	free(c->addr);
	free(c);
}

static ushort
cksum(uchar *p, int n, ulong s)
{
	while(n > 1){
		s += p[0]<<8 | p[1];
		p += 2;
		n -= 2;
	}
	if(n)
		s += p[0]<<8;
	while(s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s;
}

static int
inet(ulong a, Net *n, int nn)
{
	int i;

	for(i = 0; i < nn; i++)
		if((a & n[i].m) == n[i].a)
			return 1;
	return 0;
}

static int
isself(ulong a)
{
	int i;

	for(i = 0; i < nself; i++)
		if(self[i] == a)
			return 1;
	return 0;
}

static int
isresolver(ulong a)
{
	int i;

	for(i = 0; i < nresolvers; i++)
		if(resolvers[i] == a)
			return 1;
	return 0;
}

/* an IPv4 packet to the machine, from the gateway's address and mac */
static void
toframe(Conn *c, uchar *ip, int n)
{
	uchar f[Maxframe];

	if(n > Maxframe - Etherhdr)
		return;
	memmove(f, c->mac, 6);
	memmove(f+6, gwmac, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	memmove(f+Etherhdr, ip, n);
	sendframe(c, f, Etherhdr+n);
}

static void
arp(Conn *c, uchar *f, int n)
{
	uchar r[60], *a;

	if(n < Etherhdr+28)
		return;
	a = f+Etherhdr;
	if(a[0] != 0 || a[1] != 1 || a[2] != 0x08 || a[3] != 0 || a[4] != 6 || a[5] != 4)
		return;
	if(a[6] != 0 || a[7] != 1 || nhgetl(a+24) != (net|1))
		return;	/* only who has the gateway */
	memset(r, 0, sizeof r);
	memmove(r, a+8, 6);
	memmove(r+6, gwmac, 6);
	r[12] = 0x08;
	r[13] = 0x06;
	memmove(r+14, a, 6);
	r[20] = 0;
	r[21] = 2;
	memmove(r+22, gwmac, 6);
	hnputl(r+28, net|1);
	memmove(r+32, a+8, 6);
	memmove(r+38, a+14, 4);
	sendframe(c, r, sizeof r);
}

/* the gateway's ping */
static void
echo(Conn *c, uchar *ip, int len)
{
	uchar r[Maxframe-Etherhdr];
	int hl;

	hl = (ip[0]&15)*4;
	if(len - hl < 8 || len > sizeof r)
		return;
	memmove(r, ip, len);
	memmove(r+12, ip+16, 4);
	memmove(r+16, ip+12, 4);
	r[8] = 64;
	r[10] = r[11] = 0;
	hnputs(r+10, cksum(r, hl, 0));
	r[hl] = 0;	/* echo reply */
	r[hl+2] = r[hl+3] = 0;
	hnputs(r+hl+2, cksum(r+hl, len-hl, 0));
	toframe(c, r, len);
}

static void
dhcp(Conn *c, uchar *ip, int len)
{
	uchar r[600], *b, *o, *u, *p;
	int hl, nb, i, type, reply, ul;
	ulong yi, want;

	hl = (ip[0]&15)*4;
	b = ip+hl+8;
	nb = len - hl - 8;
	if(nb < 240 || b[0] != 1 || memcmp(b+236, "\x63\x82\x53\x63", 4) != 0)
		return;
	type = 0;
	want = nhgetl(b+12);
	for(i = 240; i < nb && b[i] != 255; ){
		if(b[i] == 0){
			i++;
			continue;
		}
		if(i+2 > nb || i+2+b[i+1] > nb)
			break;
		if(b[i] == 53 && b[i+1] >= 1)
			type = b[i+2];
		if(b[i] == 50 && b[i+1] == 4)
			want = nhgetl(b+i+2);
		i += 2 + b[i+1];
	}
	yi = net | c->lease;
	if(type == 1)
		reply = 2;	/* offer */
	else if(type == 3)
		reply = want == yi || want == 0 ? 5 : 6;	/* ack, nak */
	else
		return;

	memset(r, 0, sizeof r);
	u = r+20;
	p = u+8;
	p[0] = 2;
	p[1] = 1;
	p[2] = 6;
	memmove(p+4, b+4, 4);	/* xid */
	memmove(p+10, b+10, 2);	/* flags */
	if(reply != 6)
		hnputl(p+16, yi);
	hnputl(p+20, net|1);
	memmove(p+28, b+28, 16);	/* chaddr */
	memmove(p+236, "\x63\x82\x53\x63", 4);
	o = p+240;
	*o++ = 53; *o++ = 1; *o++ = reply;
	*o++ = 54; *o++ = 4; hnputl(o, net|1); o += 4;
	if(reply != 6){
		*o++ = 51; *o++ = 4; hnputl(o, Ttl); o += 4;
		*o++ = 1; *o++ = 4; hnputl(o, mask); o += 4;
		*o++ = 3; *o++ = 4; hnputl(o, net|1); o += 4;
		if(nresolvers > 0){
			*o++ = 6;
			*o++ = 4*nresolvers;
			for(i = 0; i < nresolvers; i++, o += 4)
				hnputl(o, resolvers[i]);
		}
	}
	*o++ = 255;
	ul = o - u;
	hnputs(u, 67);
	hnputs(u+2, 68);
	hnputs(u+4, ul);
	r[0] = 0x45;
	hnputs(r+2, 20+ul);
	r[8] = 64;
	r[9] = 17;
	hnputl(r+12, net|1);
	hnputl(r+16, 0xffffffff);
	hnputs(r+10, cksum(r, 20, 0));
	/* the UDP checksum over its pseudo-header */
	{
		ulong s;

		s = (net|1)>>16;
		s += (net|1) & 0xffff;
		s += 0xffff + 0xffff + 17 + ul;
		i = cksum(u, ul, s);
		hnputs(u+6, i == 0 ? 0xffff : i);
	}
	{
		uchar f[Maxframe];

		memset(f, 0xff, 6);
		memmove(f+6, gwmac, 6);
		f[12] = 0x08;
		f[13] = 0x00;
		memmove(f+Etherhdr, r, 20+ul);
		sendframe(c, f, Etherhdr+20+ul);
	}
	hnputl(r, yi);
	elog("%s %V dhcp %s", c->name, r, reply == 2 ? "offer" : reply == 5 ? "ack" : "nak");
}

/* a frame from the machine */
static void
fromvm(Conn *c, uchar *f, int n)
{
	uchar *ip, *l4, d[4];
	int hl, len, proto, dport;
	ulong src, dst;

	if(n < Etherhdr || n > Maxframe || (f[6] & 1) != 0)
		return;
	if(!c->hasmac){
		memmove(c->mac, f+6, 6);
		c->hasmac = 1;
	}else if(memcmp(c->mac, f+6, 6) != 0)
		return;	/* one address a port */
	if(f[12] == 0x08 && f[13] == 0x06){
		arp(c, f, n);
		return;
	}
	if(f[12] != 0x08 || f[13] != 0x00)
		return;	/* IPv6 and the rest */
	ip = f+Etherhdr;
	n -= Etherhdr;
	if(n < 20 || (ip[0]>>4) != 4)
		return;
	hl = (ip[0]&15)*4;
	len = nhgets(ip+2);
	if(hl < 20 || len < hl || len > n)
		return;
	src = nhgetl(ip+12);
	dst = nhgetl(ip+16);
	proto = ip[9];
	if(proto == 17 && len - hl >= 8 && nhgets(ip+hl+2) == 67 && (dst == 0xffffffff || dst == (net|1))){
		dhcp(c, ip, len);
		return;
	}
	if(src != (net|c->lease))
		return;	/* not its own address */
	if(dst == (net|1)){
		if(proto == 1 && len - hl >= 8 && ip[hl] == 8)
			echo(c, ip, len);
		return;
	}
	if((ip[6] & 0x3f) != 0 || ip[7] != 0)
		return;	/* a fragment */
	if(isself(dst) || inet(dst, blocked, nelem(blocked)))
		return;
	l4 = ip+hl;
	switch(proto){
	case 6:
		if(len - hl < 20)
			return;
		dport = nhgets(l4+2);
		if(dport != 80 && dport != 443 && dport != 9418 && !(dport == 53 && isresolver(dst)))
			return;
		if((l4[13] & 0x12) == 0x02){	/* SYN, not ACK: a new connection */
			if(!take(&c->syn, Synrate, Synrate, 1))
				return;
			hnputl(d, dst);
			elog("%s %V tcp %V!%d", c->name, ip+12, d, dport);
		}
		break;
	case 17:
		if(len - hl < 8 || nhgets(l4+2) != 53 || !isresolver(dst))
			return;
		break;
	case 1:
		if(len - hl < 8 || l4[0] != 8)
			return;
		break;
	default:
		return;
	}
	if(!take(&c->up, rate, Burst, len) || !daycount(c->name, len))
		return;
	c->bytes[0] += len;
	write(netfd, ip, len);
}

/* the network's packets to the machines */
static void
netproc(void*)
{
	uchar buf[8*1024];
	Conn *c;
	ulong dst;
	int n, len;

	threadsetname("ethernetd net");
	while((n = read(netfd, buf, sizeof buf)) > 0){
		if(n < 20 || (buf[0]>>4) != 4)
			continue;
		len = nhgets(buf+2);
		if(len > n || len < 20)
			continue;
		dst = nhgetl(buf+16);
		if((dst & mask) != net || (dst & ~mask) == 0 || (dst & ~mask) >= Nlease)
			continue;
		/* under lk: a connection is not let go while it is a lease */
		qlock(&lk);
		c = leases[dst & ~mask];
		if(c != nil && !c->dead && c->hasmac && take(&c->down, rate, Burst, len) && daycountl(c->name, len)){
			c->bytes[1] += len;
			toframe(c, buf, len);
		}
		qunlock(&lk);
	}
	sysfatal("the network's interface: %r");
}

static void
authconn(Conn *c)
{
	uchar chal[Nchal], buf[Maxmsg];
	char cs[64], *f[8], *why, *name;
	int n, nf;
	Ticket *t;

	genrandom(chal, sizeof chal);
	b64u(cs, sizeof cs, chal, sizeof chal);
	sendtext(c, "challenge %s", cs);
	n = read(c->fd, buf, sizeof buf - 1);
	if(n <= 1 || buf[0] != 'T')
		return;
	buf[n] = 0;
	nf = tokenize((char*)buf+1, f, nelem(f));
	name = nil;
	if(nf == 6 && strcmp(f[0], "enroll") == 0)
		why = enroll(f, cs, &name);
	else if(nf == 5 && strcmp(f[0], "login") == 0)
		why = login(f, cs, &name);
	else
		why = "bad request";
	if(why != nil){
		elog("%s: %s refused: %s", c->addr, nf > 0 ? f[0] : "-", why);
		sleep(1000);	/* slower guessing */
		sendtext(c, "error %s", why);
		return;
	}
	qlock(&lk);
	t = newticket(name);
	qunlock(&lk);
	elog("%s: %s %s", c->addr, f[0], name);
	sendtext(c, "ok %s", t->s);
	free(name);
}

static void
etherconn(Conn *c)
{
	uchar buf[Maxmsg+1];
	char *f[3];
	int n, i, l;
	Ticket *t;
	Conn *old;

	n = read(c->fd, buf, sizeof buf - 1);
	if(n <= 1 || buf[0] != 'T')
		return;
	buf[n] = 0;
	if(tokenize((char*)buf+1, f, nelem(f)) != 2 || strcmp(f[0], "ticket") != 0){
		sendtext(c, "error bad request");
		return;
	}
	qlock(&lk);
	if((t = findticket(f[1])) == nil){
		qunlock(&lk);
		elog("%s: no such ticket", c->addr);
		sendtext(c, "error no such ticket");
		return;
	}
	/* one machine a name: a new connection takes the old one's place and address, the old one ended */
	l = 0;
	for(i = 2; i < Nlease; i++)
		if((old = leases[i]) != nil && strcmp(old->name, t->name) == 0){
			old->dead = 1;
			leases[i] = nil;
			endl(old);
			l = i;
			break;
		}
	if(l == 0){
		n = 0;
		for(i = 2; i < Nlease; i++)
			if(leases[i] != nil)
				n++;
		if(n < maxmach)
			for(i = 2; i < Nlease && i < (~mask & 0xff); i++)
				if(leases[i] == nil){
					l = i;
					break;
				}
	}
	if(l == 0){
		qunlock(&lk);
		elog("%s: %s: no room", c->addr, t->name);
		sendtext(c, "error the network is full; try later");
		return;
	}
	c->t = t;
	c->name = strdup(t->name);
	c->lease = l;
	c->last = time(0);
	leases[l] = c;
	qunlock(&lk);
	hnputl(buf, net|l);
	elog("%s: %s up as %V", c->addr, c->name, buf);
	sendtext(c, "ok");

	/* until the page goes, or the timer or another connection ends it */
	while((n = read(c->fd, buf, sizeof buf)) > 0 && !c->dead)
		if(buf[0] == 'B'){
			c->last = time(0);
			fromvm(c, buf+1, n-1);
		}
	qlock(&lk);
	if(leases[c->lease] == c)
		leases[c->lease] = nil;
	c->dead = 1;
	qunlock(&lk);
	elog("%s: %s down, %lld bytes up, %lld down", c->addr, c->name, c->bytes[0], c->bytes[1]);
}

static void
connproc(void *a)
{
	Conn *c, **l;

	c = a;
	threadsetname("ethernetd %s %s", c->kind == Kauth ? "auth" : "ether", c->addr);
	if(c->kind == Kauth)
		authconn(c);
	else
		etherconn(c);
	qlock(&lk);
	for(l = &conns; *l != nil; l = &(*l)->next)
		if(*l == c){
			*l = c->next;
			break;
		}
	qunlock(&lk);
	free(c->name);
	c->name = nil;
	if(!c->ended)
		sendend(c);
	sendp(c->out, nil);	/* the writer ends it */
}

/* this machine's addresses, which the machines may not reach */
static void
readself(void)
{
	Biobuf *b;
	char *l, *f[4];
	uchar ip[IPaddrlen];

	if((b = Bopen("/net/ipselftab", OREAD)) == nil)
		return;
	while((l = Brdstr(b, '\n', 1)) != nil){
		if(tokenize(l, f, nelem(f)) >= 1 && parseip(ip, f[0]) != -1 && isv4(ip) && nself < nelem(self))
			self[nself++] = nhgetl(ip+IPv4off);
		free(l);
	}
	Bterm(b);
}

/* the default route's gateway and interface, for the machines' route out */
static void
defaultroute(void)
{
	Biobuf *b;
	char *l, *f[10];

	if(gate != nil)
		return;
	if((b = Bopen("/net/iproute", OREAD)) == nil)
		sysfatal("/net/iproute: %r");
	while((l = Brdstr(b, '\n', 1)) != nil){
		if(tokenize(l, f, nelem(f)) >= 6 && strcmp(f[0], "0.0.0.0") == 0 && strcmp(f[1], "/96") == 0 && f[3][0] == '4'){
			gate = strdup(f[2]);
			gateifc = strdup(f[5]);
			free(l);
			break;
		}
		free(l);
	}
	Bterm(b);
	if(gate == nil)
		sysfatal("no default route: -g gate ifc");
}

static void
netsetup(void)
{
	char buf[32], path[64];
	uchar a[4], m[4], r[4];
	int cfd, n, rfd;

	defaultroute();
	if((cfd = open("/net/ipifc/clone", ORDWR)) < 0)
		sysfatal("/net/ipifc/clone: %r");
	if((n = read(cfd, buf, sizeof buf - 1)) <= 0)
		sysfatal("/net/ipifc/clone: %r");
	buf[n] = 0;
	if(fprint(cfd, "bind pkt") < 0)
		sysfatal("bind pkt: %r");
	hnputl(a, net|1);
	hnputl(m, mask);
	hnputl(r, net);
	if(fprint(cfd, "add %V %V %V 1500", a, m, r) < 0)
		sysfatal("add: %r");
	if(fprint(cfd, "iprouting 1") < 0)
		sysfatal("iprouting: %r");
	snprint(path, sizeof path, "/net/ipifc/%d/data", atoi(buf));
	if((netfd = open(path, ORDWR)) < 0)
		sysfatal("%s: %r", path);
	/* the machines' route out: the default one's, translated (NAT) */
	if((rfd = open("/net/iproute", OWRITE)) < 0)
		sysfatal("/net/iproute: %r");
	hnputl(a, net);
	if(fprint(rfd, "add 0.0.0.0 0.0.0.0 %s 4t %s %V %V", gate, gateifc, a, m) < 0)
		sysfatal("the route out: %r");
	close(rfd);
	/* cfd stays open: the interface is gone when it is closed */
	elog("up: %V/%d via %s on %s", a, 24, gate, gateifc);
}

static void
usage(void)
{
	fprint(2, "usage: aux/ethernetd -r rpid -o origin [-d dir] [-s srvname] [-n net] [-g gate ifc] [-D resolver]... [-m max] [-i idle] [-R rate] [-l] [-v]\n");
	threadexitsall("usage");
}

static void
parsenet(char *s, ulong *a, ulong *m)
{
	uchar ip[IPaddrlen];
	char *p;
	int len;

	len = 32;
	if((p = strchr(s, '/')) != nil){
		*p++ = 0;
		len = atoi(p);
	}
	if(parseip(ip, s) == -1 || !isv4(ip) || len < 0 || len > 32)
		usage();
	*m = len == 0 ? 0 : ~0UL << (32-len);
	*a = nhgetl(ip+IPv4off) & *m;
}

void
threadmain(int argc, char **argv)
{
	char buf[256], *f[5], *p;
	int ctl[2], fd, n;
	uchar ip[IPaddrlen];
	Conn *c;

	fmtinstall('V', eipfmt);
	fmtinstall('I', eipfmt);
	ARGBEGIN{
	case 'r':
		rp = EARGF(usage());
		break;
	case 'o':
		authorigin = EARGF(usage());
		break;
	case 'd':
		dir = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'n':
		parsenet(EARGF(usage()), &net, &mask);
		if((~mask & 0xff) < 3)
			usage();
		break;
	case 'g':
		gate = EARGF(usage());
		gateifc = EARGF(usage());
		break;
	case 'D':
		if(nresolvers == nelem(resolvers) || parseip(ip, EARGF(usage())) == -1 || !isv4(ip))
			usage();
		resolvers[nresolvers++] = nhgetl(ip+IPv4off);
		break;
	case 'm':
		maxmach = atoi(EARGF(usage()));
		break;
	case 'R':
		rate = atof(EARGF(usage()));
		break;
	case 'l':
		logerr = 1;
		break;
	case 'i':
		idle = atoi(EARGF(usage()));
		break;
	case 'v':
		verbose = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0 || rp == nil || authorigin == nil)
		usage();
	if(nresolvers == 0){
		parseip(ip, "9.9.9.9");
		resolvers[nresolvers++] = nhgetl(ip+IPv4off);
	}
	readself();
	netsetup();

	/* the connections' ctl: etherws writes "conn SRVNAME KIND ADDR" to /srv/srvname */
	if(pipe(ctl) < 0)
		sysfatal("pipe: %r");
	snprint(buf, sizeof buf, "/srv/%s", srvname);
	remove(buf);
	if((fd = create(buf, OWRITE|ORCLOSE, 0602)) < 0)
		sysfatal("%s: %r", buf);
	if(fprint(fd, "%d", ctl[0]) < 0)
		sysfatal("%s: %r", buf);
	close(ctl[0]);
	proccreate(netproc, nil, Stack);
	proccreate(timerproc, nil, Stack);
	for(;;){
		if((n = read(ctl[1], buf, sizeof buf - 1)) <= 0)
			sysfatal("ctl: %r");
		buf[n] = 0;
		if(tokenize(buf, f, nelem(f)) != 4 || strcmp(f[0], "conn") != 0
		|| strcmp(f[2], "auth") != 0 && strcmp(f[2], "ether") != 0
		|| strchr(f[1], '/') != nil || strncmp(f[1], srvname, strlen(srvname)) != 0){
			elog("ctl: not a connection");
			continue;
		}
		p = smprint("/srv/%s", f[1]);
		fd = open(p, ORDWR);
		free(p);
		if(fd < 0){
			elog("%s: %s: %r", f[3], f[1]);
			continue;
		}
		if(verbose)
			elog("%s: %s", f[3], f[2]);
		c = mallocz(sizeof *c, 1);
		c->fd = fd;
		c->addr = strdup(f[3]);
		c->kind = strcmp(f[2], "auth") == 0 ? Kauth : Kether;
		c->out = chancreate(sizeof(Msg*), Nout);
		c->start = time(0);
		qlock(&lk);
		c->next = conns;
		conns = c;
		qunlock(&lk);
		proccreate(writeproc, c, Stack);
		proccreate(connproc, c, Stack);
	}
}
