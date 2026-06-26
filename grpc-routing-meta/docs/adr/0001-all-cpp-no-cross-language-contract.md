# 0001. All-C++; no cross-language wire contract

Status: Accepted
Date: 2026-06-27

## Context

The kit projects routing metadata out of the gRPC body into headers. The `url_encode`
and `sha256` primitives were hand-rolled originally so that a *non-C++* sender or
verifier could reproduce the exact wire bytes — a zero-dependency, cross-language byte
oracle. Grilling established that in this deployment every sender, and the side that
recomputes the digest, is C++ and shares this one kit. The `OVERVIEW.zh` §10 comparison
had already dropped "跨語言/多 sender" as unused.

## Decision

Treat the contract as **C++-only**. Do not ship a language-agnostic spec, conformance
vectors, or per-language reference implementations. **Keep** the hand-rolled
`url_encode`/`sha256` — now justified as zero-dependency *integrity* primitives, not
cross-language oracles — and **harden their tests** (NIST SHA-256 vectors, url
round-trip fuzz) rather than swapping in OpenSSL/abseil.

## Consequences

- **+** No crypto/runtime dependency; one shared implementation for sender and verifier.
- **+** Less work than adopting and pinning a crypto library.
- **−** If a non-C++ sender or verifier ever appears, this must be revisited: at that
  point the byte rules in `SPEC.md` §5–6 become the binding contract and need a
  conformance-vector suite plus a second implementation. Bounded, but real.
- The hand-rolled crypto is acceptable *only because* the digest is scoped to integrity,
  not security (see [0002](0002-digest-is-integrity-not-security.md)).
