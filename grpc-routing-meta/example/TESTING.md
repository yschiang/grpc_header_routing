# TESTING — how to verify the kit

End-to-end procedure: one build, three binaries, four pass/fail gates. Maps each
check to the invariant it proves (see `../CONTEXT.md` for the invariants).

## 0. Prerequisites

- Protobuf **3.20.3** (with libprotoc + libprotobuf) at `/Users/johnson.chiang/anaconda3` — `build.sh` points `PROTO_HOME` there.
- A C++17 compiler (`c++`). cmake optional — `build.sh` mirrors `CMakeLists.txt`.

```sh
cd example
./build.sh         # or: cmake -S . -B build && cmake --build build -j
```

## 1. Build + codegen gate — `./build.sh`

Five stages; any failure aborts with non-zero exit:

| Stage | Proves |
|---|---|
| `[gen ]` contract protos | `metadata_options` + `process_context` compile |
| `[plug]` `protoc-gen-meta` | codegen plugin links against libprotoc |
| `[neg ]` **negative codegen** | every `tests/negative/*.proto` is **rejected** by `Validate` (inv. 9, fail-loud). A fixture that gets *accepted* fails the build |
| `[gen ]` sys1/sys2/sys3 | `ProjectMeta()` generates for all 3 systems |
| `[app ]/[test]` link | the 3 binaries link the full generated set |

**Pass:** ends with `OK -> binaries in .../build/`, and `[neg ]` prints `ok (rejected):` for all three fixtures:
- `bad_repeated_scalar.proto` — `(routing.project)` on a `repeated` field
- `bad_message_project.proto` — `(routing.project)` on a message-typed field
- `bad_project_under_repeated.proto` — `(routing.project)` on a scalar under a `repeated` message

## 2. Invariant tests — `./build/test_projection`

**Pass:** prints `ALL TESTS PASSED`, exit 0. Each block in `tests/test_projection.cc`:

| Block | Inv. | Asserts |
|---|---|---|
| url + sha256 | 4 | `/`→`%2F`, space→`%20`, `?`→`%3F`, non-ASCII→UPPER hex, decode round-trips, known SHA-256 vector |
| sys1 projection | 4,6 | key-sort (`ChamberId`<`LotID`), `RecipeID=R%2FA`, `format` constant, `digest` has `sha256:` prefix, `VerifyDigest` ok, **tamper → mismatch** |
| count=0 | 7 | `count=0`, no lines, no digest |
| overflow by COUNT (30) | 8 | `overflow:true`, `format` survives, lines+digest suppressed |
| overflow by BYTES (20×~390B) | 8 | size trips overflow independent of the count cap |
| overflow by LINE (1 value >512B) | 8 | single oversized context trips the `maxline` branch |
| sys3 scalar (nested) | 9 | `x-mask-id` reached via nested walk; **empty source → throws** (required) |
| empty fields | 1 | absent fields project as `Key=` (present-but-empty); digest still round-trips |
| common headers | 2,10 | exactly 6 headers; selectors (`x-target-system`, `x-route-profile`) absent |

## 3. Round-trip digest — `./build/receiver_verify`

Projects a 2-context sys1 request, feeds the headers back through `ParseContext` + `VerifyDigest`.

**Pass:** prints `digest check: OK (header matches body)`, exit 0. Non-zero exit ⇒ header/body drift.

## 4. End-to-end shapes — `./build/unified_sender`

Five header blocks, all through the same `Send<>()`:

| Block | Expect |
|---|---|
| sys1 Calculate (2 contexts) | 6 common headers + `x-process-context-count: 2`, `-format`, `-digest: sha256:…`, two `x-process-context` lines |
| sys2 Verify (1 sparse) | `count: 1`, one context with only `RecipeID` set, rest `Key=` |
| sys2 List (count=0) | `count: 0`, `format` only — no digest, no lines |
| sys3 Submit05 | `x-mask-id: RET-9981`, `count: 0` |
| sys1 60 contexts | `count: 60`, `x-process-context-overflow: true`, no context lines |

Manual sanity on any `x-process-context` line: key-sorted (`ChamberId=` before `LotID=`) and `/`→`%2F`. Ends with `All 16 transaction types … route through the same Send<>().`

## Pass/fail summary

| Gate | Command | Pass |
|---|---|---|
| Build + negative codegen | `./build.sh` | `OK -> binaries in …`; 3 fixtures `ok (rejected)` |
| Invariants 1–10 | `./build/test_projection` | `ALL TESTS PASSED` |
| Round-trip digest | `./build/receiver_verify` | `OK (header matches body)`, exit 0 |
| E2E shapes | `./build/unified_sender` | 5 blocks; overflow flag on the 60-context block |

All four green ⇒ ship.
