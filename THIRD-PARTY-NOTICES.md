# Third-party notices

## Vendored / linked code

- **mbedTLS** (Apache-2.0) — TLS 1.2, DTLS-SRTP, AES, SHA, HKDF, X.509. Fetched by CI into `vendor/mbedtls`.
  Copyright The Mbed TLS Contributors.
- **monocypher** (CC0-1.0 / BSD-2-Clause, vendored in `vendor/monocypher`) — Curve25519 and the low-level EdDSA
  primitives used for the Signal-style signatures. Copyright Loup Vaillant and contributors.
- **zlib** (zlib license) — inflate for compressed stanzas (system library).
- **libopus** (BSD-3-Clause) — Opus audio, fetched by CI for the device build.
- **QR-Code-generator** by Nayuki (MIT) — QR rendering on the device, fetched by CI.

## Protocol references

This is an independent, clean-room-style C implementation. Its understanding of the WhatsApp protocol comes from
these public, permissively licensed projects; no code was copied from them. Their notices are reproduced in
gratitude:

- **meowcaller** (MIT) — https://github.com/purpshell/meowcaller — the WhatsApp Web VoIP call stack and the MLow
  codec, as a reference for call signaling and media framing.
- **whatsapp-rust** (MIT) — https://github.com/oxidezap/whatsapp-rust — the messaging protocol (Noise handshake,
  binary nodes, token dictionaries, pairing, client payload). The version-3 binary token tables in
  `core/wa_tokens.c` are derived from its published `tokens.json`.
- **WhatsApp Calls Research Group** — https://wacrg.org — the call-protocol specification.

## RTC stack

The WebRTC media code in `core/rtc/` (STUN / DTLS-SRTP / SCTP / RTP / RTCP) is reused from the author's own GFN6
project, also MIT-licensed.
