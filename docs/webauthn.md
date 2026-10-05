# #ω: WebAuthn

WebAuthn (passkeys) is an interface of the page's JavaScript only, so the
machine uses it through a device, as it uses the keyboard and the OPFS
disk: `sys/src/9/wasm32/devwebauthn.c`, `#ω` (a UTF-8 name, as 9front
names some devices: σ, ¤).

- `#ω/webauthn`: the machine writes a request and reads its answer on
  the same fd; the write waits for the answer (a note ends the wait).
  Requests, B64 being base64url without padding:

      create rp=RPID user=B64 name=NAME salt=B64 [challenge=B64]
      get rp=RPID salt=B64 [allow=B64[,B64...]] [challenge=B64]

  and the answer:

      ok id=B64 prf=B64|none [user=B64] [auth=B64 client=B64 sig=B64]
          [pubkey=B64 attest=B64]
      error WHY

  salt is the PRF extension's input, prf its output for the passkey -
  key material the passkey gives; `none` from an authenticator without
  PRF.
- The page asks for the user's gesture: a request shows buttons - this
  device, a phone (QR code: hints hybrid), a security key, cancel - and a
  press calls the interface.  Browsers allow WebAuthn only on a gesture,
  and the gesture tells the user what is happening.
- The rp id must be the page's host or a domain above it: the page does
  not let the machine name another site's, so a foreign page gets no
  passkey of another origin.  A config line `passkeyrp=DOMAIN` names the
  rp the machine's programs use.

The test: `d5` (`sys/src/9/wasm32/test/d5.c`) with the browser's virtual
authenticator (`tools/test/wasmapp`'s `WEBAUTHN`): a passkey made and got,
PRF the same for the same salt and another for another, a request
cancelled on the page, another domain refused.

What uses it is the system's own: Plan2001's accounts, for one, sign in
with a passkey whose PRF opens the account's keys.
