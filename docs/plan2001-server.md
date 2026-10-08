# plan2001.com's server: what the plan9-wasm32 demo needs

A request to Plan2001's side (its repository and its tools run the
server): plan2001.com is, for now, plan9-wasm32's demo only. Its
network (`docs/network.md`) needs the following on the server, and the
Plan2001-specific services go away. What plan9-wasm32's own deploy puts on
the server (`tools/site/deploy`) is listed last.

## Added

1. **DNS** (Cloudflare, by the owner):
   - `auth.plan2001.com` A 82.70.55.84, DNS only (not proxied);
   - `_acme-challenge.auth.plan2001.com` CNAME
     `_acme-challenge.auth.cpu.plan2001.com` (the server's own zone answers
     the challenge, as `_acme-challenge.plan2001.com`'s does now).
2. **The certificate**: one for both names, as tlssrv takes one: a CSR with
   the alt name (libsec's `X509rsareq` makes the names after the first
   comma subjectAltNames):

        auth/rsa2csr 'CN=plan2001.com,auth.plan2001.com' /sys/lib/tls/...key >plan2001.com.csr

   and `certrenew` renewing it with both challenges.
3. **rc-httpd** (Plan2001's `select-handler`):
   - by `$SERVER_NAME`: `auth.plan2001.com` is its own site, the
     directory `/sys/lib/plan2001/auth` (plan9-wasm32's
     `tools/site/auth.html` as its `index.html`), with the same headers
     as the machine's (COOP, COEP);
   - `GET /auth` on `auth.plan2001.com` and `GET /ether` on `plan2001.com`,
     WebSocket upgrades only:

         exec /bin/aux/etherws -r -A https://auth.plan2001.com -E https://plan2001.com

     with `remote` in its environment the browser's address, if the
     handler knows it (else etherws reads `$net/remote`, or logs `-`).
   - Everything else on `plan2001.com` as now (the front page,
     `/plan9-wasm32/`, `/iso/`).
4. **cpustart**:

        mkdir -p /adm/ether/cred; chmod 700 /adm/ether
        touch /sys/log/ether; chmod 666 /sys/log/ether
        aux/ethernetd -r auth.plan2001.com -o https://auth.plan2001.com &

   as the host owner, after the network is up (it reads the default
   route). It makes its pkt interface (100.64.0.1/24), turns on IP
   forwarding (9front's is the stack's, all interfaces), and adds a
   source-specific default route with translation for 100.64.0.0/24 only.
   Nothing else is forwarded: the cloud's security list lets only 443, 53
   and 51820 in, and the server's own services (`localonly`: 10.*,
   192.168.*) never see 100.64.0.0/24, which ethernetd does not let
   reach the server.
5. **Headers** (Codex's review, F): HSTS
   (`Strict-Transport-Security: max-age=31536000`) and
   `X-Content-Type-Options: nosniff` on every answer; on the auth site a
   strict CSP (`default-src 'self'; script-src 'self' 'unsafe-inline';
   connect-src 'self' wss://auth.plan2001.com; frame-ancestors 'none'`);
   on the machine's, CSP that lets it run (`script-src 'self' blob:
   'wasm-unsafe-eval' 'unsafe-inline'; connect-src 'self'
   wss://plan2001.com; worker-src 'self' blob:`). A limit on Range
   requests' rate (the ISO) is worth having.

## Removed

- webterm's WebSockets (rcpu, `/resume`, 567, 5356, 17019, 17030, 17040,
  17041), the app origins (`/lib/app`, `APP.cpu.plan2001.com` in the zone,
  `/app`), crsrv and the compute pool, Monolith;
- `plan2001.html` (Plan2001's machine: it shares plan2001.com's origin,
  and so its OPFS disk `sdW0`, with plan9-wasm32's machine);
- signupd, passkeyd, secstored, weblimitd, websession's accounts; the web
  accounts' data (keyfs, secstore, `/adm/webauthn`) only once the owner
  has said so.

## Kept

- tlssrv and rc-httpd (the pages);
- acmed, certrenew and ndb/dns's zone `cpu.plan2001.com` (the challenges);
- WireGuard, `localonly`, and auth on 567 for the local network and the
  tunnel (the administrator's 9pterm);
- `/iso/` and `/plan9-wasm32/` (plan9-wasm32's deploy writes them).

## plan9-wasm32's deploy

`tools/site/deploy` (over the tunnel, as now) also builds `aux/ethernetd`
and `aux/etherws` for amd64 on the server (`sys/src/cmd/ethergw`) and
installs them in `/amd64/bin/aux`, puts the auth page in
`/sys/lib/plan2001/auth/index.html`, and writes `netauth=` into
`/plan9-wasm32/plan9.ini`. It does not touch Plan2001's files above.
