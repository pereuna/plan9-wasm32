# The site's network

A machine in a browser has an Ethernet card (`#l0`, `etherwasm32.c`)
whose wire is a WebSocket to its page's origin, `/ether` (`docs/kernel.md`).
On one's own computer the other end is `tools/gateway` (passt: the
internet as the host's programs reach it; `-n` a network of its own).
On a public site the other end is the site's: `aux/ethernetd`, which lets a
machine out to the internet only after its owner has shown a passkey,
and only for the web and git.

## Trust

The machine's page is its firmware; an installed machine's firmware
comes from its own disk (`index.html` and `platform.js` on its 9fat), which
its owner can change. Any of it runs with the page's origin's rights. So
the machine's origin (`https://plan2001.com`) holds nothing that is not the
machine's own: its disk (OPFS), its BIOS settings, and a ticket for the
network. The passkeys live on another origin, `https://auth.plan2001.com`,
and their rp is that host: a browser lets a page ask only for its own
host's or a parent domain's passkeys, so the machine's page, its disk's
firmware among it, can not ask for them.

What the server trusts is the passkey's assertion and the ticket, nothing
the machine says about itself. What goes out is the server's to decide,
packet by packet.

## The way in

1. The machine boots. The site's `plan9.ini` has `netauth=URL`, the auth
   page. Without a ticket in the tab, the firmware asks, before anything
   of the disk runs: Connect the network, or Without the network (nothing
   in 10 seconds: without). The BIOS's network setting `off` never asks.
2. Connect: the tab goes to the auth page (the machine's URL kept in
   `sessionStorage` for the way back). There the person signs in with the
   passkey, or, the first time, makes one with an invitation code.
3. The auth page talks to the gateway over `wss://auth.plan2001.com/auth`:
   the gateway's challenge; the page's `enroll` (a passkey made, with an
   invitation) or `login` (one shown); the gateway's ticket.
4. The tab goes back to `https://plan2001.com/plan9-wasm32/#ticket=T`. The
   firmware keeps T in `sessionStorage` (gone with the tab) and loads the
   machine's URL as it was. Its card's WebSocket's first word is the
   ticket; the link comes up at the gateway's `ok`. `inst/start` and
   termrc see the card as on a PC.

Without a ticket there is no WebSocket: the link is down, and the machine
installs and builds from its ISO as before.

## The gateway

`aux/etherws` (one per connection, as `none`, from rc-httpd) is the
WebSocket's end: the path's origin must be the page's (`/auth` the auth
page's, `/ether` the machine's), a frame with FIN, a text at most 8K, a
binary message one Ethernet frame (14 to 1514 bytes). It hands the
connection to `aux/ethernetd` through `/srv` (its pipe, mode 0660:
the host owner's).

`aux/ethernetd` (once, as the host owner, from cpustart):

- **Invitations**: `/adm/ether/invites`, a line each, `CODE NAME`; a code is
  used once, its line removed. Without the file nobody enrolls. One
  process takes them, one at a time.
- **Passkeys**: `/adm/ether/cred/ID` (`name`, `pubkey`, `count`,
  `created`). An assertion counts when clientDataJSON's type, challenge
  and origin (the auth page's, not cross-origin) are right,
  authenticatorData's rpIdHash is the rp's, the user was present (UP) and
  verified (UV), its counter is above the last, and the signature (ES256)
  is the kept key's.
- **Tickets**: 32 random bytes, in its memory only, an hour each; while the
  machine is connected, renewed every half hour (the old one good until it
  ends). One machine a name: a new connection with the name's ticket takes
  the old one's place and its address (a page reloaded, a connection lost).
- **The machine's network**: it is a host of 100.64.0.0/24 (not the 10.* or
  192.168.* that the server's own services trust), the gateway .1, which
  answers its ARP, DHCP and ping and nothing for any other address. Its
  frames must come from one MAC address and its packets from its own IP
  address.
- **Out**: into the server's IP stack through a `pkt` interface, routed
  out with the server's address: a source-specific default route with
  9front's translation (`t`, NAT) for 100.64.0.0/24 only. Allowed: TCP to
  ports 80, 443 and 9418, DNS to the resolvers (9.9.9.9), ICMP echo. Not:
  the server's own addresses, private, shared, link-local (the cloud's
  metadata, 169.254.169.254), multicast and reserved networks, fragments,
  IPv6, other ports.
- **Limits**: 8 machines at once; 1 MB/s each way each; 20 new TCP
  connections a second; 2 GB a day a name; 15 idle minutes end a
  connection.
- **Log**: `/sys/log/ether`: passkeys made and shown, refusals, machines up
  and down with their bytes, each new TCP connection's destination.

## Invitations

On the server (the host owner):

    code=`{dd -if /dev/random -bs 16 -count 1 >[2]/dev/null | xd -x | sed 1q | sed 's/^[0-9a-f]+ //; s/ //g'}
    echo $code alice >>/adm/ether/invites
    echo $code

The code goes to the person; the name is theirs in the log and limits.
To take a person's access away: remove their `/adm/ether/cred/ID` files
(their tickets end within the hour).

## Tests

`tools/test/run ethergw` (only when named): `aux/ethernetd` and
`aux/etherws` on a wasm32 machine (A) on `tools/gateway -n`'s network;
beyond its gateway, the internet: `tools/test/netpeer` on 198.51.100.1,
ports 80, 443 and 25, reached through A's default route and its NAT.
`tools/test/etherclient` checks the protocol as a page and a machine would,
with a passkey in software: the origins, invitations (none, a wrong one, a
used one), assertions (type, challenge, origin, rp, UP, UV, counter,
signature), DHCP, ARP, ping, the NAT, the filter (port 25, the server
itself, a forged source), a full network, a ticket's second connection
taking the first's place, an idle one ended. Then a machine (B) as a person
has it: the firmware's Connect, the auth page on another origin (localhost
against 127.0.0.1), an invitation and Chromium's virtual authenticator, back
with the ticket: DHCP from the gateway, TCP to 443 out, 25 not. A's log
shows what went out.

## The server's side

The site's server is Plan2001's (9front): `docs/plan2001-server.md` is
what it needs - the auth host, its certificate, rc-httpd's routes, the
daemon in cpustart - and what goes away.
