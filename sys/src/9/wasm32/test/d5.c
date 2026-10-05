#include <u.h>
#include <libc.h>

/*
 * #W, the page's WebAuthn (devwebauthn.c, docs/webauthn.md), with the test's
 * browser's virtual authenticator (tools/test-wasmapp WEBAUTHN: its PRF,
 * and the page's buttons clicked): a passkey for glenda made, then got -
 * its PRF output 32 bytes, the same for the same salt and another for
 * another, its user handle glenda without the id given; a request
 * cancelled on the page, and one for a domain not the page's.  What it
 * says has no ids or keys in it: they differ each time.
 *	d5 RP
 */
enum { Nans = 16*1024 };

static char *rp;
static char *salt1 = "plan2001 test salt 1";
static char *salt2 = "plan2001 test salt 2";

/* base64url, no padding, as WebAuthn's */
static char*
b64u(uchar *p, int n)
{
	char *s, *t;

	s = malloc(2*n + 8);
	enc64(s, 2*n + 8, p, n);
	for(t = s; *t; t++)
		if(*t == '+')
			*t = '-';
		else if(*t == '/')
			*t = '_';
		else if(*t == '='){
			*t = 0;
			break;
		}
	return s;
}

static int
unb64u(char *s, uchar *p, int n)
{
	char *t, *u;
	int m;

	m = strlen(s);
	t = malloc(m + 4);
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

/* the request, its answer (alloc'd) */
static char*
ask(char *fmt, ...)
{
	char req[1024], *ans;
	va_list arg;
	int fd, n;

	va_start(arg, fmt);
	vsnprint(req, sizeof req, fmt, arg);
	va_end(arg);
	if((fd = open("#W/webauthn", ORDWR)) < 0)
		sysfatal("#W/webauthn: %r");
	if(write(fd, req, strlen(req)) < 0){
		ans = smprint("write error: %r");
		close(fd);
		return ans;
	}
	ans = mallocz(Nans, 1);
	n = read(fd, ans, Nans-1);
	close(fd);
	if(n < 0)
		snprint(ans, Nans, "read error: %r");
	return ans;
}

/* the answer's NAME=VALUE, alloc'd; nil none */
static char*
field(char *ans, char *name)
{
	char *f[32], *s;
	int i, n, l;

	s = strdup(ans);
	n = tokenize(s, f, nelem(f));
	l = strlen(name);
	for(i = 1; i < n; i++)
		if(strncmp(f[i], name, l) == 0 && f[i][l] == '='){
			name = strdup(f[i]+l+1);
			free(s);
			return name;
		}
	free(s);
	return nil;
}

static int
ok(char *what, char *ans)
{
	if(strncmp(ans, "ok ", 3) == 0)
		return 1;
	print("%s: %s\n", what, ans);
	return 0;
}

void
main(int argc, char **argv)
{
	char *a, *id, *prf1, *prf2, *prf, *u, *s1, *s2, *user;
	uchar b[64];

	if(argc != 2){
		fprint(2, "usage: d5 rp\n");
		exits("usage");
	}
	rp = argv[1];
	user = b64u((uchar*)"glenda", 6);
	s1 = b64u((uchar*)salt1, strlen(salt1));
	s2 = b64u((uchar*)salt2, strlen(salt2));

	a = ask("create rp=%s user=%s name=glenda salt=%s", rp, user, s1);
	if(!ok("create", a))
		exits("create");
	id = field(a, "id");
	print("create: ok, prf %s\n", strcmp(field(a, "prfok"), "1") == 0 ? "enabled" : "not enabled");

	a = ask("get rp=%s salt=%s allow=%s", rp, s1, id);
	if(!ok("get", a))
		exits("get");
	prf1 = field(a, "prf");
	u = field(a, "user");
	print("get: %s id, %d-byte prf, user %s\n", strcmp(field(a, "id"), id) == 0 ? "the same" : "another",
		prf1 && strcmp(prf1, "none") != 0 ? unb64u(prf1, b, sizeof b) : 0,
		u && unb64u(u, b, sizeof b) == 6 && memcmp(b, "glenda", 6) == 0 ? "glenda" : "?");

	a = ask("get rp=%s salt=%s allow=%s", rp, s1, id);
	if(ok("again", a)){
		prf = field(a, "prf");
		print("again: %s prf\n", strcmp(prf, prf1) == 0 ? "the same" : "another");
	}
	a = ask("get rp=%s salt=%s allow=%s", rp, s2, id);
	if(ok("other salt", a)){
		prf2 = field(a, "prf");
		print("other salt: %s prf\n", strcmp(prf2, prf1) == 0 ? "the same" : "another");
	}
	/* discoverable: no id given, the passkey says whose it is */
	a = ask("get rp=%s salt=%s", rp, s1);
	if(ok("discoverable", a)){
		u = field(a, "user");
		print("discoverable: user %s, %s prf\n",
			u && unb64u(u, b, sizeof b) == 6 && memcmp(b, "glenda", 6) == 0 ? "glenda" : "?",
			strcmp(field(a, "prf"), prf1) == 0 ? "the same" : "another");
	}
	a = ask("get rp=%s salt=%s allow=%s", rp, s1, id);
	print("cancel: %s\n", a);
	a = ask("get rp=example.com salt=%s", s1);
	print("other rp: %s\n", a);
	a = ask("put rp=%s", rp);
	print("bad request: %s\n", a);
	exits(nil);
}
