# Wazzup

An unofficial WhatsApp client for jailbroken **iOS 6**, built as a WhatsApp **linked device** (the same role as
WhatsApp Web and WhatsApp Desktop) — no proxy or server of any kind, everything runs inside the app. A hobby
interoperability project in the same family as the other iOS 6 "revival" apps.

**The headline feature is calls: 1:1 voice and video, incoming and outgoing.** Messaging (text, groups, media,
voice notes) comes with it.

## How it works

The app links to the user's account by showing a QR code that the phone scans (WhatsApp → Linked devices). From
then on it speaks WhatsApp's own protocol directly:

- **Transport:** a TLS 1.2 WebSocket to `web.whatsapp.com`, with the **Noise XX** handshake (Curve25519 + AES-GCM +
  SHA-256) inside it, exactly like the web/desktop clients. The server's Noise certificate chain is verified
  against WhatsApp's own root key.
- **Stanzas:** WhatsApp's binary-XML ("WAP") nodes with the version-3 token dictionaries.
- **Pairing & identity:** Signal-style Curve25519 identity, signed pre-keys, and the ADV signed-device-identity
  exchange.
- **Calls (the goal):** `<call>` signaling (offer / preaccept / accept / transport / terminate) with a per-device
  Signal-encrypted call key, then SRTP media to WhatsApp's relays over a WebRTC-style DTLS + SCTP data channel —
  reusing the proven plain-C WebRTC stack from the GFN6 project (STUN / DTLS-SRTP / SCTP / RTP / RTCP on mbedTLS,
  hardware H.264 decode via private VideoToolbox). Audio is MLow (receivers also decode Opus); video is H.264.

Everything is plain C99 on mbedTLS + monocypher, so one source tree builds for iOS 6 (armv7), newer iOS (arm64)
and a desktop test harness.

## Layout

```
core/        the protocol core (C): util, crypto, protobuf, binary nodes, Noise, net, store, pairing, client driver
core/rtc/    the WebRTC media stack (STUN/DTLS/SRTP/SCTP/RTP), reused from GFN6
vendor/      monocypher (vendored); mbedTLS is fetched by CI into vendor/mbedtls
desktop/     the offline self-test harness and a pairing CLI (for development, run by the author)
src/         the iOS app (UI) — to come
tools/       build/push scripts
```

## Building & testing

The author has no local C toolchain; **everything compiles in GitHub Actions**. CI builds the core on a Linux
runner and runs the **offline self-tests** (crypto vectors, binary-node round trip, protobuf, client payload) — no
network and no account. Live pairing and calls are tested from the device.

## Status

Early. Transport, Noise handshake, binary nodes, protobuf, device identity and QR pairing are implemented; call
signaling and media are next. See `CHANGELOG.md`.

## Account safety

This is an unofficial client, so it carries a ban risk, which the author accepts for their own account. It never
sends messages or places calls on its own — those are user-initiated.

## License

MIT (see `LICENSE`). Third-party components and protocol references are credited in `THIRD-PARTY-NOTICES.md`.
