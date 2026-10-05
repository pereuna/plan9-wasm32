#include <u.h>
#include <libc.h>
#include <mp.h>
#include <libsec.h>
#include <authsrv.h>
#include <auth.h>

/*
 * (tools/test-9wasm32) D3's libraries by 3c: libsec's digests, ciphers,
 * key derivations and curve25519, libmp's arithmetic, and libauthsrv's
 * dp9ik pieces - passtokey, AuthPAK and form1 tickets - against values
 * an independent implementation gives (crypto.out: Python's hashlib and
 * cryptography).  AuthPAK and form1 are checked against themselves: the
 * two sides agree, a wrong password does not, a ticket comes back.
 *
 * d3 -p: dp9ik end to end through factotum - a client (user glenda) and
 * a server (user bootes) authenticate over a pipe with p9any (auth_proxy
 * on both sides); the users differ, so the client's factotum must get
 * the tickets from the auth server (net!p9auth.dom!ticket: /net/cs, a
 * WebSocket, tools/test-authsrv or the CPU VM's) - it makes them itself
 * only for the same user.  The two get the same AuthInfo and secret.
 *
 * d3 -n rounds procs: /net/tcp reconnected - each proc connects one
 * conversation to the auth server again and again (connect, an AuthPAK
 * request, the answer, hangup on the same ctl): every answer whole,
 * AuthOK and a PAK y, none another connection's; and a reader waiting in
 * data wakes when another proc hangs up.
 *
 * d3 -r host: dp9ik with 9front itself - glenda authenticates to the CPU
 * VM's rcpu (p9any: its factotum, its auth server) and gets her AuthInfo.
 */

static void
hex(char *name, uchar *p, int n)
{
	print("%s %.*H\n", name, n, p);
}

static uchar msg[] = "The quick brown fox jumps over the lazy dog";
static uchar key32[32], iv16[16], nonce12[12];

static void
digests(void)
{
	uchar d[SHA2_512dlen], *big;
	DigestState *s;
	int i;

	md5(msg, sizeof msg-1, d, nil); hex("md5", d, MD5dlen);
	sha1(msg, sizeof msg-1, d, nil); hex("sha1", d, SHA1dlen);
	sha2_256(msg, sizeof msg-1, d, nil); hex("sha256", d, SHA2_256dlen);
	sha2_512(msg, sizeof msg-1, d, nil); hex("sha512", d, SHA2_512dlen);
	/* a million bytes in pieces of 999: the block code's carries */
	big = malloc(1000000);
	for(i = 0; i < 1000000; i++)
		big[i] = i*7;
	s = nil;
	for(i = 0; i+999 < 1000000; i += 999)
		s = sha2_256(big+i, 999, nil, s);
	sha2_256(big+i, 1000000-i, d, s);
	hex("sha256-1M", d, SHA2_256dlen);
	sha1(big, 1000000, d, nil); hex("sha1-1M", d, SHA1dlen);
	free(big);
	hmac_sha1(msg, sizeof msg-1, key32, 20, d, nil); hex("hmac-sha1", d, SHA1dlen);
	hmac_sha2_256(msg, sizeof msg-1, key32, 32, d, nil); hex("hmac-sha256", d, SHA2_256dlen);
}

static void
ciphers(void)
{
	uchar buf[64], tag[16];
	AESstate aes;
	Chachastate cs;

	memmove(buf, msg, 43);
	memset(buf+43, 5, 5);	/* 48 bytes, padded */
	setupAESstate(&aes, key32, 16, iv16);
	aesCBCencrypt(buf, 48, &aes);
	hex("aes128-cbc", buf, 48);
	memmove(buf, msg, 43);
	memset(buf+43, 5, 5);
	setupAESstate(&aes, key32, 32, iv16);
	aesCBCencrypt(buf, 48, &aes);
	hex("aes256-cbc", buf, 48);

	memmove(buf, msg, 43);
	setupChachastate(&cs, key32, 32, nonce12, 12, 20);
	chacha_encrypt(buf, 43, &cs);
	hex("chacha20", buf, 43);

	memmove(buf, msg, 43);
	setupChachastate(&cs, key32, 32, nonce12, 12, 20);
	ccpoly_encrypt(buf, 43, (uchar*)"aad", 3, tag, &cs);
	hex("ccpoly", buf, 43);
	hex("ccpoly-tag", tag, 16);
}

static void
kdfs(void)
{
	uchar d[64], pub[32], sec[32], base[32];
	int i;

	pbkdf2_x((uchar*)"password", 8, (uchar*)"salt", 4, 4096, d, 20, hmac_sha1, SHA1dlen);
	hex("pbkdf2-sha1", d, 20);
	hkdf_x((uchar*)"salt", 4, (uchar*)"info", 4, key32, 32, d, 42, hmac_sha2_256, SHA2_256dlen);
	hex("hkdf-sha256", d, 42);

	for(i = 0; i < 32; i++)
		sec[i] = 0x40+i;
	memset(base, 0, 32);
	base[0] = 9;
	curve25519(pub, sec, base);
	hex("x25519-pub", pub, 32);
}

static void
mp(void)
{
	mpint *b, *e, *m, *r;

	fmtinstall('B', mpfmt);
	b = strtomp("123456789abcdef0fedcba9876543210", nil, 16, nil);
	e = mpnew(0);
	mpleft(mpone, 200, e);
	mpadd(e, mpone, e);
	m = mpnew(0);
	mpleft(mpone, 521, m);
	mpsub(m, mpone, m);
	r = mpnew(0);
	mpexp(b, e, m, r);
	print("mpexp %B\n", r);
	mpmul(b, b, r);
	mpmul(r, b, r);
	print("mpcube %B\n", r);
	mpdiv(m, b, r, e);
	print("mpdiv %B %B\n", r, e);
}

static void
dp9ik(void)
{
	Authkey c, s, bad;
	PAKpriv pc, ps, pb;
	uchar yc[PAKYLEN], ys[PAKYLEN], yb[PAKYLEN];
	Ticket t, t2;
	char m[MAXTICKETLEN];
	int n;

	passtokey(&c, "a dp9ik password");
	hex("passtoaeskey", c.aes, AESKEYLEN);
	hex("passtodeskey", (uchar*)c.des, DESKEYLEN);

	/* AuthPAK: client and server from the same password and user agree */
	s = c;
	authpak_hash(&c, "glenda");
	authpak_hash(&s, "glenda");
	authpak_new(&pc, &c, yc, 1);
	authpak_new(&ps, &s, ys, 0);
	if(authpak_finish(&pc, &c, ys) < 0 || authpak_finish(&ps, &s, yc) < 0)
		print("authpak: finish failed\n");
	else
		print("authpak: keys %s\n", tsmemcmp(c.pakkey, s.pakkey, PAKKEYLEN) == 0 ? "agree" : "differ");
	/* a wrong password: the keys differ */
	passtokey(&bad, "a wrong password");
	authpak_hash(&bad, "glenda");
	s = c;
	passtokey(&s, "a dp9ik password");
	authpak_hash(&s, "glenda");
	authpak_new(&pb, &bad, yb, 1);
	authpak_new(&ps, &s, ys, 0);
	if(authpak_finish(&pb, &bad, ys) < 0 || authpak_finish(&ps, &s, yb) < 0)
		print("authpak wrong password: finish failed\n");
	else
		print("authpak wrong password: keys %s\n", tsmemcmp(bad.pakkey, s.pakkey, PAKKEYLEN) == 0 ? "agree" : "differ");

	/* a form1 ticket (ccpoly under the PAK key) there and back */
	memset(&t, 0, sizeof t);
	t.num = AuthTs;
	memmove(t.chal, "12345678", CHALLEN);
	strcpy(t.cuid, "glenda");
	strcpy(t.suid, "glenda");
	memset(t.key, 0x5a, NONCELEN);
	t.form = 1;
	n = convT2M(&t, m, sizeof m, &c);
	memset(&t2, 0, sizeof t2);
	if(convM2T(m, n, &t2, &c) != n || t2.num != AuthTs || strcmp(t2.cuid, "glenda") != 0
	|| memcmp(t2.key, t.key, NONCELEN) != 0)
		print("form1: ticket lost\n");
	else
		print("form1: ticket back, %d bytes\n", n);
	m[n/2] ^= 1;
	print("form1 changed: %s\n", convM2T(m, n, &t2, &c) == n && t2.num == AuthTs ? "taken" : "refused");
}

static void
proxy(char *dom)
{
	int p[2], q[2], n;
	AuthInfo *ai;
	uchar sec[256];
	char buf[ERRMAX];

	if(pipe(p) < 0 || pipe(q) < 0)
		sysfatal("pipe: %r");
	switch(fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		close(p[0]);
		close(q[0]);
		ai = auth_proxy(p[1], nil, "proto=p9any role=server dom=%s user=bootes", dom);
		if(ai == nil){
			fprint(q[1], "server: %r");
			exits("server");
		}
		print("server: cuid %s suid %s, a %d-byte secret\n", ai->cuid, ai->suid, ai->nsecret);
		write(q[1], ai->secret, ai->nsecret);
		exits(nil);
	}
	close(p[1]);
	close(q[1]);
	ai = auth_proxy(p[0], nil, "proto=p9any role=client dom=%s user=glenda", dom);
	if(ai == nil)
		print("client: %r\n");
	else
		print("client: cuid %s suid %s, a %d-byte secret\n", ai->cuid, ai->suid, ai->nsecret);
	close(p[0]);
	n = read(q[0], sec, sizeof sec);
	waitpid();
	if(ai == nil)
		return;
	if(n != ai->nsecret || memcmp(sec, ai->secret, n) != 0){
		if(n > 0 && n < ERRMAX && memcmp(sec, "server: ", 8) == 0){
			memmove(buf, sec, n);
			buf[n] = 0;
			print("%s\n", buf);
		} else
			print("the secrets differ\n");
	} else
		print("the secrets agree\n");
	auth_freeAI(ai);
}

static int
pakreq(int data)
{
	Ticketreq tr;
	char buf[TICKREQLEN];
	uchar y[1+PAKYLEN];
	int n;

	memset(&tr, 0, sizeof tr);
	tr.type = AuthPAK;
	strcpy(tr.authid, "bootes");
	strcpy(tr.authdom, "plan2001");
	strcpy(tr.hostid, "glenda");
	strcpy(tr.uid, "glenda");
	n = convTR2M(&tr, buf, sizeof buf);
	if(write(data, buf, n) != n)
		return -1;
	if(readn(data, y, sizeof y) != sizeof y || y[0] != AuthOK)
		return -1;
	return 0;
}

static int
conn(int ctl, char *dir)
{
	char buf[64];
	int data;

	if(fprint(ctl, "connect p9auth.plan2001!567") < 0)
		return -1;
	snprint(buf, sizeof buf, "%s/data", dir);
	if((data = open(buf, ORDWR)) < 0)
		return -1;
	return data;
}

static void
reconnect(int rounds, int procs)
{
	char dir[40], buf[16];
	int i, j, ctl, data, n, bad;

	for(j = 0; j < procs; j++){
		switch(fork()){
		case -1:
			sysfatal("fork: %r");
		case 0:
			if((ctl = open("/net/tcp/clone", ORDWR)) < 0 || (n = read(ctl, buf, sizeof buf-1)) <= 0)
				sysfatal("clone: %r");
			buf[n] = 0;
			snprint(dir, sizeof dir, "/net/tcp/%d", atoi(buf));
			bad = 0;
			for(i = 0; i < rounds; i++){
				if((data = conn(ctl, dir)) < 0 || pakreq(data) < 0)
					bad++;
				if(data >= 0)
					close(data);
				fprint(ctl, "hangup");
			}
			close(ctl);
			if(bad)
				print("proc %d: %d of %d rounds bad: %r\n", j, bad, rounds);
			exits(bad ? "bad" : nil);
		}
	}
	bad = 0;
	for(j = 0; j < procs; j++){
		Waitmsg *w = wait();
		if(w == nil || w->msg[0])
			bad++;
		free(w);
	}
	print("reconnect: %d procs, %d rounds each, %d bad\n", procs, rounds, bad);

	/* a reader waiting in data wakes when another proc hangs up */
	if((ctl = open("/net/tcp/clone", ORDWR)) < 0 || (n = read(ctl, buf, sizeof buf-1)) <= 0)
		sysfatal("clone: %r");
	buf[n] = 0;
	snprint(dir, sizeof dir, "/net/tcp/%d", atoi(buf));
	if((data = conn(ctl, dir)) < 0)
		sysfatal("connect: %r");
	switch(fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		sleep(1000);
		fprint(ctl, "hangup");
		exits(nil);
	}
	n = read(data, buf, sizeof buf);
	waitpid();
	print("reader after hangup: read %d\n", n);
	/* and the conversation connects again */
	close(data);
	if((data = conn(ctl, dir)) < 0 || pakreq(data) < 0)
		print("again: %r\n");
	else
		print("connected again\n");
}

static void
rcpu(char *host)
{
	int fd;
	AuthInfo *ai;

	if((fd = dial(netmkaddr(host, "net", "rcpu"), nil, nil, nil)) < 0)
		sysfatal("dial: %r");
	ai = auth_proxy(fd, nil, "proto=p9any role=client user=glenda");
	if(ai == nil)
		print("rcpu: %r\n");
	else
		print("rcpu: cuid %s suid %s, a %d-byte secret\n", ai->cuid, ai->suid, ai->nsecret);
	close(fd);
}

void
main(int argc, char **argv)
{
	int i;

	if(argc == 3 && strcmp(argv[1], "-p") == 0){
		proxy(argv[2]);
		exits(nil);
	}
	if(argc == 4 && strcmp(argv[1], "-n") == 0){
		reconnect(atoi(argv[2]), atoi(argv[3]));
		exits(nil);
	}
	if(argc == 3 && strcmp(argv[1], "-r") == 0){
		rcpu(argv[2]);
		exits(nil);
	}

	fmtinstall('H', encodefmt);
	for(i = 0; i < 32; i++)
		key32[i] = i;
	for(i = 0; i < 16; i++)
		iv16[i] = 0xf0+i;
	for(i = 0; i < 12; i++)
		nonce12[i] = 0xa0+i;
	digests();
	ciphers();
	kdfs();
	mp();
	dp9ik();
	print("done\n");
	exits(nil);
}
