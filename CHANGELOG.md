# Changelog

## Unreleased

### Added — protocol core (plain C, builds for iOS 6 armv7 / arm64 / desktop)
- `wa_util` — byte buffers, base64/hex, time, CSPRNG, logging.
- `wa_crypto` — SHA-256/512, MD5, HMAC, HKDF-SHA256, AES-GCM/CBC/CTR (mbedTLS) and Curve25519 DH + Signal-style
  XEdDSA signatures (monocypher primitives). Self-tested against the libsignal signature vector and RFC 5869.
- `wa_proto` — minimal protobuf writer/reader by field number.
- `wa_node` + `wa_tokens` — WhatsApp binary-XML ("WAP") encode/decode with the version-3 token dictionaries,
  JID packing (JID_PAIR / AD_JID), packed nibble/hex. Round-trip self-tested.
- `wa_noise` — the Noise XX handshake WhatsApp runs inside its WebSocket, with server-certificate verification
  against WhatsApp's root key, plus the transport ciphers and 3-byte frame helpers.
- `wa_net` — a TLS 1.2 WebSocket client (mbedTLS) and a plain HTTPS GET.
- `wa_store` — the device identity (registration id, Noise/identity/signed-pre keys, ADV secret) with a flat
  save/load, and the registration + login `ClientPayload` builder.
- `wa_pair` — linked-device QR pairing: the QR string and the ADV signed-device-identity verify + counter-sign.
- `wa_client` — the connection driver: connect, handshake, encrypt/decrypt stanzas, run the QR pairing exchange,
  keepalive, reconnect.
- `core/rtc/` — the GFN6 WebRTC media stack (STUN / DTLS-SRTP / SCTP / RTP / RTCP), for the call media to come.
- `desktop/` — an offline self-test harness (run in CI) and a development pairing CLI.
- CI: compiles the core on Linux and runs the offline self-tests on every push.

### Next
- `<call>` signaling (offer / preaccept / accept / transport / terminate) and SRTP media over the relay.
- Messaging: Signal sessions (libsignal-protocol in C), history sync, media.
- The iOS 6 app UI (skeuomorphic; the call screen like the iOS 6 Phone/FaceTime screen).
