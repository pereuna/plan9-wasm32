#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #ω: the browser's WebAuthn (passkeys) for the machine (docs/webauthn.md).
 * ω, a UTF-8 name, as 9front names some of its devices (σ, ¤).
 * The API is the page's (navigator.credentials), as the keyboard and the
 * disk are: the machine writes a request to #ω/webauthn, the page shows a
 * button - a browser wants a person's gesture for a passkey, and the
 * button says what is asked - and its answer is read back on the same fd.
 * The write waits for the answer (a note ends the wait and the page's
 * button goes); one request at a time, the page's.
 *
 *	create rp=RPID user=B64 name=NAME salt=B64 [challenge=B64]
 *	get rp=RPID salt=B64 [allow=B64[,B64...]] [challenge=B64]
 *
 * B64 is base64url without padding, as WebAuthn's.  salt is the PRF
 * extension's input; the answer has its output for this passkey:
 *
 *	ok id=B64 prf=B64|none [user=B64] [auth=B64 client=B64 sig=B64]
 *		[pubkey=B64 attest=B64]
 *	error WHY
 *
 * The page checks that rp is its own host or a domain above it: the
 * browser would refuse another anyway.
 */
enum
{
	Qtop	= 0,
	Qwebauthn,

	Maxreq	= 4096,
	Maxresp	= 16*1024,
};

typedef struct Answer Answer;
struct Answer
{
	long	word;		/* the page's: 0 waiting, its length, -1 none */
	char	buf[Maxresp];
};

typedef struct Read Read;
struct Read		/* an fd's answer, read from its start */
{
	char	*s;
	long	off;
};

static struct
{
	QLock;		/* one request at a time */
	ulong	gen;	/* the page's request: an old one's answer is not taken */
} wa;

static Dirtab watab[] = {
	".",		{Qtop, 0, QTDIR},	0,	0555,
	"webauthn",	{Qwebauthn},		0,	0666,
};

static Chan*
waattach(char *spec)
{
	return devattach(L'ω', spec);
}

static Walkqid*
wawalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, watab, nelem(watab), devgen);
}

static int
wastat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, watab, nelem(watab), devgen);
}

static Chan*
waopen(Chan *c, int omode)
{
	c = devopen(c, omode, watab, nelem(watab), devgen);
	c->aux = nil;	/* this fd's answer */
	return c;
}

static void
waclose(Chan *c)
{
	Read *r;

	if((r = c->aux) != nil){
		memset(r->s, 0, strlen(r->s));	/* PRF outputs are keys */
		free(r);
		c->aux = nil;
	}
}

static long
waread(Chan *c, void *a, long n, vlong)
{
	Read *r;

	switch((ulong)c->qid.path){
	case Qtop:
		return devdirread(c, a, n, watab, nelem(watab), devgen);
	case Qwebauthn:
		/* from the answer's start, whatever the write did to the fd's offset */
		if((r = c->aux) == nil)
			return 0;
		n = readstr(r->off, a, n, r->s);
		r->off += n;
		return n;
	}
	error(Egreg);
	return 0;
}

/* a request's words: create or get, then NAME=VALUE in the characters B64 and names have */
static void
checkreq(char *s, long n)
{
	long i;

	if(n >= 7 && memcmp(s, "create ", 7) == 0 || n >= 4 && memcmp(s, "get ", 4) == 0){
		for(i = 0; i < n; i++)
			if(strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_=,. ", s[i]) == nil)
				error(Ebadarg);
		return;
	}
	error(Ebadctl);
}

static long
wawrite(Chan *c, void *a, long n, vlong)
{
	char *req;
	Answer *ans;
	Read *r;
	ulong gen;
	long m;

	if((ulong)c->qid.path != Qwebauthn)
		error(Eperm);
	if(n <= 0 || n >= Maxreq)
		error(Etoobig);
	req = smalloc(n+1);
	if(waserror()){
		free(req);
		nexterror();
	}
	memmove(req, a, n);
	for(m = n; m > 0 && req[m-1] == '\n'; m--)
		;
	req[m] = 0;
	checkreq(req, m);
	/* the answer's memory is the page's to write until it says it is done: never freed */
	ans = xalloc(sizeof *ans);
	if(ans == nil)
		error(Enomem);
	eqlock(&wa);
	if(waserror()){
		qunlock(&wa);
		nexterror();
	}
	gen = ++wa.gen;
	ans->word = 0;
	platwebauthn(gen, req, ans->buf, sizeof ans->buf - 1, &ans->word);
	if(waserror()){	/* a note: the page's button goes, its answer is not taken */
		platwebauthn(gen, nil, nil, 0, nil);
		nexterror();
	}
	while(ans->word == 0)
		tsleep(&up->sleep, return0, nil, 50);
	poperror();
	if(ans->word < 0 || ans->word >= sizeof ans->buf)
		error("no answer from the page");
	ans->buf[ans->word] = 0;
	r = smalloc(sizeof *r + ans->word + 1);
	r->s = (char*)(r+1);
	memmove(r->s, ans->buf, ans->word+1);
	r->off = 0;
	waclose(c);
	c->aux = r;
	poperror();
	qunlock(&wa);
	memset(ans->buf, 0, sizeof ans->buf);
	xfree(ans);
	poperror();
	free(req);
	return n;
}

Dev webauthndevtab = {
	L'ω',
	"webauthn",

	devreset,
	devinit,
	devshutdown,
	waattach,
	wawalk,
	wastat,
	waopen,
	devcreate,
	waclose,
	waread,
	devbread,
	wawrite,
	devbwrite,
	devremove,
	devwstat,
};
