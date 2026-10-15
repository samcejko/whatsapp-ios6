# Changelog

## 0.1.0 — the app

WhatsApp6 builds as an installable iOS app (armv7 for iOS 6, arm64 for iOS 14+) with the whole protocol stack and
a skeuomorphic iOS 6 UI. CI compiles both architectures and runs the offline self-tests (10 groups, all green).

### The app (Objective-C, Theos)
- `WASession` — bridges the C core (`wa_client`) to the UI: owns the device identity, runs the connection on a
  background thread, republishes QR / state / paired / node events as notifications.
- Screens: pairing with a live QR (scan in WhatsApp → Linked devices), the chat list with a connection banner,
  a chat view with glossy green/white speech bubbles and an input bar, and a Phone/FaceTime-style call screen
  with round glossy controls. `WATheme` is the skeuomorphic iOS 6 look; strings in English + Czech.
- `WAAudioEngine` — VoiceProcessingIO (echo cancellation) at 16 kHz with Opus encode/decode and a jitter buffer.
- Theos build: one tree → armv7 (iOS 6, iOS 9.3 SDK) and arm64 (iOS 14+, iOS 16.5 SDK) packages; icon + launch
  images from `tools/icon.svg`; VoIP background mode and mic/camera usage strings.

### Live transport
- `wa_rtc` — the call media transport: UDP → DTLS → SCTP → the pre-negotiated WebRTC data channel to a relay,
  reusing the GFN6 `gf_dtls`/`gf_sctp` stack. (Exercised only against a real relay, not in CI.)
- `wa_message` — the Message protobuf (text and the call-key wrapper), Signal encryption + WhatsApp padding, and
  the `<message>`/`<enc>` stanza; end-to-end self-test (text + call key, A→B through the stanza).
- `wa_signal` — the Signal protocol in C: X3DH session from a prekey bundle and the Double Ratchet
  (SignalMessage / PreKeySignalMessage, skipped-key handling); A↔B + out-of-order self-test.

### Messaging + calls wired end to end (in code)
- Client IQ request/response correlation + an action queue (`wa_client_post` / `wa_client_request_sync`).
- `wa_user` (usync device list + prekey fetch/parse), `wa_sessions` (persisted Signal session store), `wa_chat`
  (resolve devices → establish sessions → encrypt per device → DM fanout send; incoming decrypt; call-key enc),
  `wa_callengine` (place / accept / reject / terminate; incoming-offer call-key decrypt).
- The app sends and receives real encrypted text; the chat list and chat view update; call buttons send a real
  encrypted `<offer>`; the call screen opens the echo-cancelled 16 kHz Opus audio engine.

### MLow (inbound codec) — entropy + framing + LSF stage done, synthesis staged
- `wa_mlow_rc` — the Opus/CELT range decoder (ec_dec) in C, **bit-exact** against 2000 + 1500 KAT ops.
- `wa_mlow` — the TOC parse (per-frame Opus / MLow / DTX routing), KAT over all 256 bytes; the audio engine
  routes each inbound frame accordingly. **Opus frames decode and play.**
- `wa_mlow` LSF stage — the stage-1 selector, grid and 16 stage-2 residual indices, **KAT vs lsf_vectors** with
  the runtime CDF tables embedded from the captured `smpl_tables.json`.
- The remaining MLow smpl stages (pitch/LTP, pulses, gains, comfort-noise, LSF→LPC, CELP synthesis, post-filters)
  are a large interconnected codec: later stages rebuild the decoder's runtime memory image from seed blobs and
  the audio-synthesis vectors are only published compressed. Porting them to a verified, bit-exact decoder is a
  multi-session effort. **It is only needed to hear the OFFICIAL client (which sends MLow); two WhatsApp6 devices
  call each other with Opus and need no MLow DSP.**

### Remaining (device, deferred by the user)
- Live pairing with the user's number; the call media loop end to end (STUN allocate over `wa_rtc` + routing the
  SRTP-wrapped audio) verified on the device; the MLow speech-DSP decoder (a large staged port).

## Earlier (protocol core)

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
- `wa_srtp` — the call media crypto (E2E 1:1 path): SRTP session keys from the 32-byte call key + participant LID
  (HKDF-SHA256 → AES-CM PRF), the RTP AES-CTR IV, payload encryption, the 4-byte WARP MESSAGE-INTEGRITY tag
  (HMAC-SHA1), and send/recv ROC trackers. **KAT-verified in CI** against the meowcaller / whatsapp-rust vectors
  (peer/self keys, RTP IV, ciphertext, WARP tag all match byte-for-byte).
- `wa_call` — 1:1 call signaling: builders for the `<call>` stanzas (offer / preaccept / accept / transport /
  terminate / reject / mute_v2 / relaylatency / heartbeat) with the load-bearing `<offer>` child order, plus
  offer parsing. Round-trip + child-order self-test.
- `wa_wstun` — WhatsApp's STUN flavour for the relay handshake: ALLOCATE/BINDING with a RELAY-TOKEN,
  MESSAGE-INTEGRITY (HMAC-SHA1), FINGERPRINT (CRC-32), the keepalive ping, the XOR relay endpoint and the native
  sender-subscription attribute, plus parsing. **KAT-verified** (attr, MI+FP messages, ping, xor-endpoint match).
- `wa_relay` — parses the `<relay>` node (te2 endpoints with ipv4/ipv6+port, indexed `<token>`/`<auth_token>`,
  `<hbh_key>`, `<warp_mi_tag_len>`) and picks the media endpoint (FNA-first for an inbound call).
- `desktop/` — an offline self-test harness (run in CI) and a development pairing CLI.
- CI: compiles the core on Linux and runs the offline self-tests on every push (now 8 groups, all green).

### Next
- Wire the RTC stack (`core/rtc/` gf_dtls/gf_sctp) to the WhatsApp relay: UDP → DTLS → SCTP → pre-negotiated
  DataChannel (id 0, label "pre-negotiated"), STUN ALLOCATE with the relay token, then WARP-framed SRTP media.
  The crypto, signaling, STUN and relay parsing for it are all in place and KAT-verified.
- Per-device `<enc>` call-key encryption via Signal sessions (libsignal-protocol in C) — shared with messaging.
- Audio (MLow decode + Opus, VoiceProcessingIO at 16 kHz) and the H.264 hardware-encode spike on the device.
- The iOS 6 app UI (skeuomorphic; the call screen like the iOS 6 Phone/FaceTime screen).
