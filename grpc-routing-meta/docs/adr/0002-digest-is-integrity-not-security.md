# 0002. Digest is integrity, not security

Status: Accepted
Date: 2026-06-27

## Context

`x-process-context-digest` is `sha256:` + SHA-256 over the canonical, key-sorted,
newline-joined process-context lines; the receiver recomputes and compares. Two questions:
(a) must it defend against *adversarial* tampering, or only *accidental* drift? (b) given
all-C++ with one shared kit (ADR 0001), does a digest earn its place at all?

## Decision

Scope the digest to **accidental drift only — integrity, not authenticity**. Keep plain
SHA-256: **no** HMAC, no key, no signature. **Keep** the digest (do not drop it) because
the sender and the verifier are *independently deployed* services whose projection logic
can skew between kit versions; the digest catches that skew, plus transit mangling.

## Consequences

- **+** No key material, no rotation, no key-distribution problem.
- **−** Provides **no** protection against a party who can edit the body — they recompute
  the hash. Any future authenticity requirement is a wire + ops change (HMAC or signing
  with key rotation), not a tweak.
- Docs MUST state "integrity, not security" (`SPEC.md` §5.3, `CONTEXT.md` invariant 6) so
  no downstream component over-trusts the digest as a tamper-proof seal.
- Plain hand-rolled SHA-256 is acceptable precisely because this is not a security control
  (see ADR 0001).
