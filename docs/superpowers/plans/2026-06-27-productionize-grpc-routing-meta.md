# Productionize grpc-routing-meta Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take the example-grade `grpc-routing-meta` kit to production grade so every benefit claimed in `refs/OVERVIEW.zh.md` §7/§10 is demonstrably true, meeting all `refs/BRIEF.md` acceptance criteria A–I.

**Architecture:** Pivot the projection API from `void`/throw to `ProjResult{ok, issues[], duration}` (report-don't-dictate), generated `ProjectMeta` moves into `namespace routingmeta` and times itself; missing required scalar → `x-routing-error` header (no throw); overflow → non-blocking issue. De-hardcode the build, add a CI matrix, a perf bench, and align docs with code.

**Tech Stack:** C++17, protobuf + libprotoc (`protoc-gen-meta` plugin), hand-rolled url_encode/sha256 (kept, not swapped), GitHub Actions.

## Global Constraints

- **C++17.** No new third-party dependencies. KEEP the hand-rolled `url_encode.h`/`sha256.h` (plan.md: harden tests, don't swap in OpenSSL).
- **Protobuf** with `libprotoc`; must work on **protobuf 3.20.3 and 3.21.12**.
- **No throw on a data condition** (SPEC §7). Required-missing and overflow are reported, not thrown.
- **Wire format is SPEC's** (`refs/SPEC.md` wins on bytes). Error header is exactly `x-routing-error: missing:<key>` (e.g. `missing:x-mask-id`).
- **Policy constants live in one place** — `process_context_emit.h`: `7168` / `25` / `512`. Don't duplicate.
- **Generated `ProjectMeta`** lives in `namespace routingmeta`, returns `routingmeta::ProjResult`, resolved by ADL via the `MetadataSink&` argument.
- **Commits:** concise, imperative, **no `Co-Authored-By` trailer**. Commit locally as you go; **do not push**.
- **`refs/` is read-only.** Docs to edit are the live copies under `grpc-routing-meta/`.
- All build/run commands are from `grpc-routing-meta/example/` unless stated. The standard test cycle is `./build.sh && ./build/test_projection` (build.sh rebuilds the plugin, regenerates code, runs the negative-codegen gate, links every binary).

---

### Task 1: `EmitProcessContexts` returns an overflow signal

Decouple overflow reporting from the projection result: the helper returns `true` iff it suppressed the context lines. The plugin (Task 2) maps that bool to a `ProjResult` issue. Changing `void`→`bool` is backward-compatible with the current generated 2-arg call sites (they ignore the return).

**Files:**
- Modify: `grpc-routing-meta/example/src/common/process_context_emit.h:38-72`
- Test: `grpc-routing-meta/example/tests/test_projection.cc` (add a block in `main`)

**Interfaces:**
- Produces: `bool routingmeta::EmitProcessContexts(MetadataSink&, const std::vector<std::string>& ctxs)` — returns `true` iff overflow (lines + digest suppressed, `x-process-context-overflow: true` emitted); `false` otherwise (including `count=0`).

- [ ] **Step 1: Write the failing test**

Add just before the final `std::printf("ALL TESTS PASSED\n");` in `tests/test_projection.cc`:

```cpp
  // --- EmitProcessContexts returns overflow signal (Task 1) ---
  {
    std::vector<std::string> few(2, "ChamberId=CH-A");
    routingmeta::VectorSink sink;
    bool of = routingmeta::EmitProcessContexts(sink, few);
    assert(!of);                                                  // within budget
    assert(sink.Get("x-process-context-digest").rfind("sha256:", 0) == 0);

    std::vector<std::string> many(30, "ChamberId=CH-A");
    routingmeta::VectorSink sink2;
    bool of2 = routingmeta::EmitProcessContexts(sink2, many);
    assert(of2);                                                  // count > 25
    assert(sink2.Get("x-process-context-overflow") == "true");
    assert(sink2.Count("x-process-context") == 0);
  }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd grpc-routing-meta/example && ./build.sh`
Expected: FAIL compiling `test_projection` — `void value not ignored as it ought to be` (assigning `void` to `bool of`).

- [ ] **Step 3: Make `EmitProcessContexts` return `bool`**

In `src/common/process_context_emit.h`, change the signature and the three return points:

```cpp
inline bool EmitProcessContexts(MetadataSink& sink, const std::vector<std::string>& ctxs) {
  sink.Add("x-process-context-count", std::to_string(ctxs.size()));
  sink.Add("x-process-context-format", "urlencoded-query-string-v1");
  if (ctxs.empty()) return false;                            // count=0: nothing to project

  static const std::string kKey = "x-process-context";
  size_t projected = 24 + 71 + kHpackEntryOverhead;
  size_t maxline = 0;
  for (const auto& c : ctxs) {
    projected += kKey.size() + c.size() + kHpackEntryOverhead;
    maxline = std::max(maxline, c.size());
  }

  const bool overflow = ctxs.size() > kMaxContexts
                     || maxline > kMaxLineBytes
                     || sink.bytes() + projected > kMaxTotalMetaBytes;
  if (overflow) {
    sink.Add("x-process-context-overflow", "true");          // explicit, never silent
    return true;                                             // backend reads full detail from body
  }

  std::string canon;
  for (size_t i = 0; i < ctxs.size(); ++i) {
    if (i) canon.push_back('\n');
    canon += ctxs[i];
  }
  sink.Add("x-process-context-digest", "sha256:" + Sha256Hex(canon));
  for (const auto& c : ctxs) sink.Add(kKey, c);
  return false;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/test_projection`
Expected: `ALL TESTS PASSED`. (The current generated `*.proj.cc` calls `EmitProcessContexts(sink, ctxs);` ignoring the now-`bool` return — still compiles.)

- [ ] **Step 5: Commit**

```bash
git add grpc-routing-meta/example/src/common/process_context_emit.h grpc-routing-meta/example/tests/test_projection.cc
git commit -m "emit: EmitProcessContexts returns overflow signal"
```

---

### Task 2: `ProjResult` type + plugin pivot (no throw, `x-routing-error`, self-timed, `namespace routingmeta`)

The core change. Generated `ProjectMeta` returns `ProjResult`, lives in `namespace routingmeta`, times its own body, and on a missing required scalar emits `x-routing-error` instead of throwing. This forces the test rewrite of invariant 9 and the overflow cases.

**Files:**
- Create: `grpc-routing-meta/example/src/common/proj_result.h`
- Modify: `grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc:172-251` (the two emission blocks + includes)
- Modify: `grpc-routing-meta/example/tests/test_projection.cc` (sys3 block + overflow asserts + duration)

**Interfaces:**
- Consumes: `routingmeta::EmitProcessContexts(...) -> bool` (Task 1).
- Produces:
  - `namespace routingmeta { struct Issue { enum Kind { MissingRequired, Overflow } kind; std::string key; }; struct ProjResult { bool ok = true; std::vector<Issue> issues; std::chrono::nanoseconds duration{}; }; }`
  - Generated, per request type: `routingmeta::ProjResult routingmeta::ProjectMeta(const <pkg>::<Msg>& req, routingmeta::MetadataSink& sink)`.

- [ ] **Step 1: Create the `ProjResult` type**

Create `src/common/proj_result.h`:

```cpp
// ProjResult — what a projection reports back (SPEC §7: report, don't dictate).
// The kit never throws on a data condition and never logs; the caller inspects
// `ok`/`issues` and decides (abort vs proceed), and reads `duration` for tracing.
#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace routingmeta {

struct Issue {
  enum Kind { MissingRequired, Overflow } kind;
  std::string key;   // header key for MissingRequired (e.g. "x-mask-id"); empty for Overflow
};

struct ProjResult {
  bool ok = true;                       // false iff a blocking issue (MissingRequired)
  std::vector<Issue> issues;            // non-blocking issues (Overflow) keep ok = true
  std::chrono::nanoseconds duration{};  // wall time of the projection (criterion H)
};

}  // namespace routingmeta
```

- [ ] **Step 2: Write the failing tests (rewrite invariant 9 + overflow + duration)**

In `tests/test_projection.cc`, add the include near the others:

```cpp
#include "common/proj_result.h"
```

Replace the **sys3 scalar block** (the one that currently asserts `threw`) with:

```cpp
  // --- sys3 scalar projection (nested) + missing-required -> ProjResult, NO throw (inv. 9) ---
  {
    sys3::v1::Submit05Request req;
    req.mutable_job()->mutable_mask()->set_mask_id("RET-9981");
    routingmeta::VectorSink sink;
    routingmeta::ProjResult r = ProjectMeta(req, sink);
    assert(r.ok);
    assert(r.issues.empty());
    assert(r.duration.count() > 0);                                // self-timed (criterion H)
    assert(sink.Get("x-mask-id") == "RET-9981");                   // reached via nested walk
    assert(sink.Get("x-process-context-count") == "0");

    sys3::v1::Submit05Request empty;                               // mask not set
    routingmeta::VectorSink s2;
    routingmeta::ProjResult r2 = ProjectMeta(empty, s2);           // MUST NOT throw
    assert(!r2.ok);
    assert(r2.issues.size() == 1 && r2.issues[0].kind == routingmeta::Issue::MissingRequired);
    assert(r2.issues[0].key == "x-mask-id");
    assert(s2.Get("x-routing-error") == "missing:x-mask-id");      // explicit, in-band
    assert(s2.Get("x-mask-id").empty());                           // empty header NOT emitted
  }
```

In the **overflow-by-COUNT block**, capture and assert the issue:

```cpp
  // --- overflow by COUNT (>25) ---
  {
    auto req = sys1Req(30);
    routingmeta::VectorSink sink;
    routingmeta::ProjResult r = ProjectMeta(req, sink);
    assert(r.ok);                                                  // overflow is non-blocking
    assert(r.issues.size() == 1 && r.issues[0].kind == routingmeta::Issue::Overflow);
    assert(sink.Get("x-process-context-count") == "30");
    assert(sink.Get("x-process-context-format") == "urlencoded-query-string-v1");
    assert(sink.Get("x-process-context-overflow") == "true");
    assert(sink.Count("x-process-context") == 0);
    assert(sink.Get("x-process-context-digest").empty());
  }
```

Remove the now-unused `#include <stdexcept>` if present (the throw test is gone).

- [ ] **Step 3: Run to verify it fails**

Run: `cd grpc-routing-meta/example && ./build.sh`
Expected: FAIL — the current generated `ProjectMeta` returns `void`, so `routingmeta::ProjResult r = ProjectMeta(...)` won't compile, and `routingmeta::Issue` is referenced but the generated header doesn't expose `ProjResult` yet.

- [ ] **Step 4: Rewrite the plugin emission**

In `src/plugin/protoc-gen-meta.cc`, change the **`.proj.h` emission block** (currently around lines 172–188) to declare in `namespace routingmeta` and include `proj_result.h`:

```cpp
    {
      std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".proj.h"));
      io::Printer p(os.get(), '$');
      p.Print(
        "// AUTO-GENERATED by protoc-gen-meta. DO NOT EDIT.\n"
        "#pragma once\n"
        "#include \"$b$.pb.h\"\n"
        "#include \"common/metadata_sink.h\"\n"
        "#include \"common/proj_result.h\"\n\n"
        "namespace routingmeta {\n",
        "b", base);
      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        std::vector<Proj> projs; walkProj(d, "", &projs);
        if (projs.empty() && !FindCtx(d)) continue;
        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink);\n",
                "ns", ns, "m", d->name());
      }
      p.Print("}  // namespace routingmeta\n");
    }
```

Change the **`.proj.cc` emission block** (currently around lines 190–251) to open `namespace routingmeta`, return `ProjResult`, self-time, drop the throw, emit `x-routing-error`, and map `EmitProcessContexts`'s bool to an `Overflow` issue:

```cpp
    {
      std::unique_ptr<io::ZeroCopyOutputStream> os(ctx->Open(base + ".proj.cc"));
      io::Printer p(os.get(), '$');
      p.Print(
        "// AUTO-GENERATED by protoc-gen-meta. DO NOT EDIT.\n"
        "// Writes ONLY body-derived metadata; sender fills the common headers.\n"
        "#include \"$b$.proj.h\"\n"
        "#include \"common/url_encode.h\"\n"
        "#include \"common/process_context_emit.h\"\n"
        "#include <chrono>\n#include <vector>\n#include <string>\n\n"
        "namespace routingmeta {\n\n",
        "b", base);

      for (int i = 0; i < file->message_type_count(); ++i) {
        const Descriptor* d = file->message_type(i);
        std::vector<Proj> projs; walkProj(d, "", &projs);
        const FieldDescriptor* ctxf = FindCtx(d);
        if (projs.empty() && !ctxf) continue;

        p.Print("ProjResult ProjectMeta(const $ns$::$m$& req, MetadataSink& sink) {\n"
                "  ProjResult _r;\n"
                "  const auto _t0 = std::chrono::steady_clock::now();\n",
                "ns", ns, "m", d->name());

        for (const auto& pj : projs) {
          std::string v = "req." + pj.getter;
          if (pj.required) {
            // required + empty -> report (no throw), emit x-routing-error, omit the scalar.
            p.Print(
              "  if ($v$.empty()) {\n"
              "    _r.ok = false;\n"
              "    _r.issues.push_back({Issue::MissingRequired, \"$k$\"});\n"
              "    sink.Add(\"x-routing-error\", \"missing:$k$\");\n"
              "  } else {\n"
              "    sink.Add(\"$k$\", UrlEncode($v$));\n"
              "  }\n",
              "v", v, "k", pj.key);
          } else {
            p.Print("  if (!$v$.empty()) sink.Add(\"$k$\", UrlEncode($v$));\n",
                    "v", v, "k", pj.key);
          }
        }

        if (ctxf) {
          std::vector<std::pair<std::string, std::string>> cf;
          const Descriptor* cm = ctxf->message_type();
          for (int k = 0; k < cm->field_count(); ++k) {
            const FieldDescriptor* sf = cm->field(k);
            if (!sf->options().HasExtension(routing::pctx)) continue;
            cf.push_back({sf->options().GetExtension(routing::pctx).key(), sf->name() + "()"});
          }
          std::sort(cf.begin(), cf.end());
          p.Print("  {\n    std::vector<std::string> ctxs;\n    for (const auto& e : req.$r$()) {\n      std::string s;\n",
                  "r", ctxf->name());
          for (size_t j = 0; j < cf.size(); ++j) {
            const std::string sep = (j == 0) ? "" : "&";
            p.Print("      s += \"$sep$$k$=\"; s += UrlEncode(e.$g$);\n",
                    "sep", sep, "k", cf[j].first, "g", cf[j].second);
          }
          p.Print("      ctxs.push_back(std::move(s));\n    }\n");
          // policy (count/format/digest/overflow + 7KB guard) is in the shared helper;
          // it returns true iff it suppressed the lines -> record a non-blocking issue.
          p.Print("    if (EmitProcessContexts(sink, ctxs)) _r.issues.push_back({Issue::Overflow, \"\"});\n  }\n");
        }
        p.Print("  _r.duration = std::chrono::steady_clock::now() - _t0;\n  return _r;\n}\n\n");
      }
      p.Print("}  // namespace routingmeta\n");
    }
```

(Inside `namespace routingmeta` the generated body refers to `UrlEncode`, `Issue`, `ProjResult`, `MetadataSink`, `EmitProcessContexts` unqualified — they're all in that namespace.)

- [ ] **Step 5: Run to verify it passes**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/test_projection`
Expected: negative gate still `ok (rejected)` ×3; `ALL TESTS PASSED`.

Also confirm the apps still build & run (they ignore the now-`ProjResult` return; `Send` in `unified_sender.cc` is still its local `void` template, unchanged here):

Run: `./build/unified_sender >/dev/null && ./build/receiver_verify`
Expected: receiver prints `digest check: OK (header matches body)`.

- [ ] **Step 6: Commit**

```bash
git add grpc-routing-meta/example/src/common/proj_result.h grpc-routing-meta/example/src/plugin/protoc-gen-meta.cc grpc-routing-meta/example/tests/test_projection.cc
git commit -m "feat: ProjectMeta returns ProjResult (no throw; x-routing-error on missing required)"
```

---

### Task 3: Promote `Send<>()` into the kit

Move the unified `Send` template out of the demo app into a kit header so it's part of the product (plan.md: "promote `Send` into kit"). It returns `ProjResult`.

**Files:**
- Create: `grpc-routing-meta/example/src/common/send.h`
- Test: `grpc-routing-meta/example/tests/test_projection.cc` (add a block; add include)

**Interfaces:**
- Consumes: `FillCommon(const Runtime&, MetadataSink&)`, `routingmeta::ProjectMeta(...) -> ProjResult`.
- Produces: `template <class Req> routingmeta::ProjResult routingmeta::Send(const Req&, const Runtime&, MetadataSink&)`.

- [ ] **Step 1: Write the failing test**

In `tests/test_projection.cc` add the include:

```cpp
#include "common/send.h"
```

Add a block before `ALL TESTS PASSED`:

```cpp
  // --- Send<>() = FillCommon + ProjectMeta, one path, returns ProjResult (inv. 10) ---
  {
    auto req = sys1Req(2);
    routingmeta::VectorSink sink;
    routingmeta::ProjResult r = routingmeta::Send(req, Runtime{"CORR-S", "F18", "ETCH01"}, sink);
    assert(r.ok);
    assert(sink.Get("x-contract-version") == "v1");                // common headers present
    assert(sink.Get("x-process-context-count") == "2");            // projection present
    assert(r.duration.count() > 0);
  }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd grpc-routing-meta/example && ./build.sh`
Expected: FAIL — `common/send.h: No such file or directory`.

- [ ] **Step 3: Create `send.h`**

Create `src/common/send.h`:

```cpp
// Send<>() — the one call a client makes: common headers + body projection, timed.
// ProjectMeta is resolved by ADL at instantiation (the caller's TU includes the
// relevant generated *.proj.h). No per-system/method branching — the overload is
// chosen by Req. Returns ProjResult: the caller inspects ok/issues and decides.
#pragma once
#include "common/common_headers.h"
#include "common/metadata_sink.h"
#include "common/proj_result.h"

namespace routingmeta {

template <class Req>
ProjResult Send(const Req& req, const Runtime& rt, MetadataSink& sink) {
  FillCommon(rt, sink);
  return ProjectMeta(req, sink);
}

}  // namespace routingmeta
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/test_projection`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add grpc-routing-meta/example/src/common/send.h grpc-routing-meta/example/tests/test_projection.cc
git commit -m "feat: promote Send<>() into the kit (returns ProjResult)"
```

---

### Task 4: `unified_sender` uses the kit `Send` and demos the empty-mask failure path

Make the demo use the kit's `Send` (delete the local copy) and add the criterion-C demonstration: an empty sys3 mask → `x-routing-error` + `duration` printed. `dump` now shows `ok`, issues, and duration.

**Files:**
- Modify: `grpc-routing-meta/example/sender/unified_sender.cc`

**Interfaces:**
- Consumes: `routingmeta::Send(...) -> ProjResult` (Task 3).

- [ ] **Step 1: Replace the local `Send` + `dump` with kit `Send` + result-aware `dump`**

In `sender/unified_sender.cc`: add `#include "common/send.h"` next to the other includes, and **delete** the local `template <class Req> void Send(...)` definition (lines ~32–36). Replace the `dump` function with one that takes the `ProjResult`:

```cpp
// [demo] Only prints what got attached; not part of adoption.
static void dump(const char* title, const routingmeta::VectorSink& s,
                 const routingmeta::ProjResult& r) {
  const double us = r.duration.count() / 1000.0;
  std::printf("=== %s   (%zu bytes metadata, ok=%s, %.2f us) ===\n",
              title, s.bytes(), r.ok ? "true" : "false", us);
  for (const auto& kv : s.items)
    std::printf("  %-26s %s\n", (kv.first + ":").c_str(), kv.second.c_str());
  for (const auto& is : r.issues)
    std::printf("  [issue] %s%s\n",
                is.kind == routingmeta::Issue::MissingRequired ? "missing-required " : "overflow ",
                is.key.c_str());
  std::printf("\n");
}
```

- [ ] **Step 2: Update each call site to capture the result**

Each demo block changes `Send(req, rt, sink);` + `dump(title, sink);` to capture and pass `r`. Example (sys1 block):

```cpp
    routingmeta::VectorSink sink;                  // [+meta] in prod: routingmeta::GrpcSink sink(&ctx);
    routingmeta::ProjResult r = Send(req, rt, sink);  // [+meta] the only added call
    dump("sys1  Calculate (2 contexts)", sink, r);     // [demo]
```

Apply the same `auto r = Send(...); dump(..., sink, r);` shape to the sys2-verify, sys2-list, sys3-Submit05, and sys1-overflow blocks. (`Send` is ADL-resolved via the `routingmeta::VectorSink` argument — keep the call unqualified to preserve the "two lines" adoption story.)

- [ ] **Step 3: Add the empty-mask failure demo (criterion C)**

Add a new block after the existing sys3 Submit05 block:

```cpp
  // --- sys3  empty mask id -> x-routing-error + duration (no throw) ---
  {
    sys3::v1::Submit05Request req;                          // [app] mask deliberately NOT set
    routingmeta::VectorSink sink;                          // [+meta]
    routingmeta::ProjResult r =
        Send(req, Runtime{"CORR-sys3-005", "F18", "LITHO01"}, sink);  // [+meta]
    dump("sys3 Submit05 (EMPTY mask -> x-routing-error)", sink, r);   // [demo] ok=false, issue, duration
  }
```

- [ ] **Step 4: Build and verify the demonstration**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/unified_sender`
Expected: every block prints a `... us)` duration; the new block shows
`x-routing-error: missing:x-mask-id`, `[issue] missing-required x-mask-id`, and `ok=false`.

- [ ] **Step 5: Commit**

```bash
git add grpc-routing-meta/example/sender/unified_sender.cc
git commit -m "sender: use kit Send; demo empty mask -> x-routing-error + duration"
```

---

### Task 5: Portable build — de-hardcode `build.sh`

Remove the `anaconda3` hardcode (criterion A). Discover the toolchain via `pkg-config protobuf` when available, else derive the prefix from `protoc`'s location. Env overrides (`PROTOC`, `CXX`, `CXXFLAGS`) honored.

**Files:**
- Modify: `grpc-routing-meta/example/build.sh:6-22`

- [ ] **Step 1: Replace the hardcoded toolchain block**

Replace lines 6–22 (`ROOT=...` through the `PBFLAGS=(...)` line) with:

```bash
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Toolchain discovery — no hardcoded paths (criterion A). Override with env if needed:
#   PROTOC=/path/to/protoc CXX=clang++ ./build.sh
PROTOC="${PROTOC:-protoc}"
command -v "$PROTOC" >/dev/null 2>&1 || { echo "error: protoc not found (set PROTOC=...)"; exit 1; }

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists protobuf; then
  PB_INC="$(pkg-config --variable=includedir protobuf)"
  PB_CFLAGS="$(pkg-config --cflags protobuf)"
  PB_LIBS="$(pkg-config --libs protobuf)"
else
  # Derive <prefix> from <prefix>/bin/protoc (works for system, Homebrew, conda).
  PREFIX="$(cd "$(dirname "$(command -v "$PROTOC")")/.." && pwd)"
  PB_INC="$PREFIX/include"
  PB_CFLAGS="-I$PB_INC"
  PB_LIBS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib -lprotobuf"
fi

CXX="${CXX:-c++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -Wall}"
GEN="$ROOT/build/generated"
BIN="$ROOT/build"

# -I dirs must be a textual prefix of the file args below, so keep proto/ relative.
IPROTO=(-I proto -I "$PB_INC")
PBFLAGS=(-I "$GEN" -I "$ROOT/src" $PB_CFLAGS $PB_LIBS)
```

Update the **plugin link** (currently lines ~41–45) to use the discovered flags and add `-lprotoc`:

```bash
echo "[plug] protoc-gen-meta"
$CXX $CXXFLAGS \
  src/plugin/protoc-gen-meta.cc "$GEN/metadata_options.pb.cc" \
  -I "$GEN" $PB_CFLAGS \
  -lprotoc $PB_LIBS \
  -o "$BIN/protoc-gen-meta"
```

(`-lprotoc` precedes `$PB_LIBS`/`-lprotobuf`: libprotoc depends on libprotobuf, so the dependent lib must link first — matters for static protobuf. The `-L` in `$PB_LIBS` is global to GNU/ld64 and still resolves `-lprotoc`.)

- [ ] **Step 2: Verify no hardcoded path remains**

Run: `grep -n anaconda grpc-routing-meta/example/build.sh || echo "clean"`
Expected: `clean`.

- [ ] **Step 3: Build green via discovered toolchain**

Run: `cd grpc-routing-meta/example && PROTOC="$(command -v protoc)" ./build.sh && ./build/test_projection`
Expected: full build, negative gate passes, `ALL TESTS PASSED`. (Locally `protoc` resolves from `PATH`; the prefix-derive branch finds its include/lib.)

- [ ] **Step 4: Commit**

```bash
git add grpc-routing-meta/example/build.sh
git commit -m "build: de-hardcode build.sh (pkg-config / prefix discovery)"
```

---

### Task 6: Perf micro-bench

Add `bench_projection` (criterion H): per-call `duration` for 1/2/25/60 contexts, sub-ms assert. Wire into `build.sh` and `CMakeLists.txt`.

**Files:**
- Create: `grpc-routing-meta/example/tests/bench_projection.cc`
- Modify: `grpc-routing-meta/example/build.sh` (add a build step)
- Modify: `grpc-routing-meta/example/CMakeLists.txt` (add a target)

**Interfaces:**
- Consumes: `routingmeta::ProjectMeta(...) -> ProjResult` with `.duration`.

- [ ] **Step 1: Write the bench (it asserts sub-ms — failing until it exists)**

Create `tests/bench_projection.cc`:

```cpp
// =============================================================================
// bench_projection — criterion H: report per-call projection time. Prints
// average us/call over many iterations for 1/2/25/60 contexts and asserts the
// per-call duration stays sub-millisecond. Uses the same self-timed duration the
// kit reports to callers (ProjResult::duration).
// =============================================================================
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "common/metadata_sink.h"
#include "common/proj_result.h"
#include "sys1.proj.h"

static sys1::v1::CalculateRequest makeReq(int n) {
  sys1::v1::CalculateRequest req;
  req.set_tool_id("ETCH01");
  for (int i = 0; i < n; ++i) {
    auto* c = req.add_contexts();
    c->set_lot_id("LOT" + std::to_string(i));
    c->set_chamber_id("CH-A"); c->set_recipe_id("RCP_ETCH_V3"); c->set_tech("N5");
    c->set_part_id("PART-A"); c->set_stage_id("ETCH"); c->set_operation_no("OP100");
  }
  return req;
}

int main() {
  const int kIters = 2000;
  for (int n : {1, 2, 25, 60}) {
    auto req = makeReq(n);
    std::chrono::nanoseconds total{};
    routingmeta::ProjResult last;
    for (int i = 0; i < kIters; ++i) {
      routingmeta::VectorSink sink;
      last = routingmeta::ProjectMeta(req, sink);
      total += last.duration;
    }
    const double us = total.count() / 1000.0 / kIters;
    std::printf("%3d contexts: %7.3f us/call (last %7.3f us)\n",
                n, us, last.duration.count() / 1000.0);
    assert(us < 1000.0);   // sub-millisecond (criterion H)
  }
  std::printf("BENCH OK (sub-ms)\n");
  return 0;
}
```

- [ ] **Step 2: Wire into `build.sh`**

After the `[test] test_projection` line in `build.sh`, add:

```bash
echo "[bench] bench_projection"
$CXX $CXXFLAGS tests/bench_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/bench_projection"
```

- [ ] **Step 3: Wire into `CMakeLists.txt`**

After the `add_test(NAME projection ...)` line, add:

```cmake
add_executable(bench_projection tests/bench_projection.cc)
target_link_libraries(bench_projection PRIVATE routing_meta_gen)
add_test(NAME bench COMMAND bench_projection)
```

- [ ] **Step 4: Build and run the bench**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/bench_projection`
Expected: four lines of `us/call` (all well under 1000) then `BENCH OK (sub-ms)`.

- [ ] **Step 5: Commit**

```bash
git add grpc-routing-meta/example/tests/bench_projection.cc grpc-routing-meta/example/build.sh grpc-routing-meta/example/CMakeLists.txt
git commit -m "bench: per-call duration micro-bench (1/2/25/60 contexts, sub-ms)"
```

---

### Task 7: CI matrix (GitHub Actions)

Criterion B: Linux × {gcc, clang} × {protobuf 3.20.3, 3.21.12} running both build paths, the negative gate, the binaries + tests + bench; plus a cheap GrpcSink compile-smoke job. Cannot be executed or pushed from this workspace — authored to be green-on-push and validated by YAML/inspection.

**Files:**
- Create: `.github/workflows/ci.yml` (workspace root)

- [ ] **Step 1: Write the workflow**

Create `.github/workflows/ci.yml`:

```yaml
name: ci
on:
  push:
  pull_request:

jobs:
  matrix:
    name: ${{ matrix.cc }} · protobuf ${{ matrix.protobuf }}
    runs-on: ubuntu-22.04
    strategy:
      fail-fast: false
      matrix:
        cc: [gcc, clang]
        protobuf: ["3.20.3", "3.21.12"]
    steps:
      - uses: actions/checkout@v4

      - name: Install build tools
        run: |
          sudo apt-get update
          sudo apt-get install -y build-essential cmake autoconf automake libtool curl pkg-config clang

      - name: Select compiler
        run: |
          if [ "${{ matrix.cc }}" = "clang" ]; then
            echo "CC=clang"   >> "$GITHUB_ENV"; echo "CXX=clang++" >> "$GITHUB_ENV"
          else
            echo "CC=gcc"     >> "$GITHUB_ENV"; echo "CXX=g++"     >> "$GITHUB_ENV"
          fi

      - name: Build & install protobuf ${{ matrix.protobuf }}
        run: |
          V="${{ matrix.protobuf }}"
          curl -fsSL -o protobuf.tar.gz \
            "https://github.com/protocolbuffers/protobuf/releases/download/v${V}/protobuf-cpp-${V}.tar.gz"
          tar xf protobuf.tar.gz
          cd "protobuf-${V}"
          ./configure --prefix=/usr/local CXX="$CXX"
          make -j"$(nproc)"
          sudo make install
          sudo ldconfig

      - name: build.sh (plugin + negative gate + binaries)
        working-directory: grpc-routing-meta/example
        run: ./build.sh

      - name: Run binaries, tests, bench (build.sh artifacts)
        working-directory: grpc-routing-meta/example
        run: |
          ./build/unified_sender
          ./build/receiver_verify
          ./build/test_projection
          ./build/bench_projection

      - name: CMake build
        working-directory: grpc-routing-meta/example
        run: |
          cmake -S . -B build-cmake -DCMAKE_CXX_COMPILER="$CXX"
          cmake --build build-cmake -j"$(nproc)"

      - name: CMake tests
        working-directory: grpc-routing-meta/example
        run: ctest --test-dir build-cmake --output-on-failure

  grpc-smoke:
    name: GrpcSink compile-smoke
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install protobuf + grpc dev
        run: |
          sudo apt-get update
          sudo apt-get install -y build-essential pkg-config \
            protobuf-compiler libprotobuf-dev libprotoc-dev libgrpc++-dev
      - name: Compile & smoke GrpcSink
        working-directory: grpc-routing-meta/example
        run: |
          cat > grpc_smoke.cc <<'EOF'
          #define ROUTINGMETA_WITH_GRPC
          #include "common/metadata_sink.h"
          int main() {
            grpc::ClientContext ctx;
            routingmeta::GrpcSink sink(&ctx);
            sink.Add("x-test", "1");          // exercises the byte-tracking Add + GrpcSink::Write
            return 0;
          }
          EOF
          g++ -std=c++17 -I src grpc_smoke.cc $(pkg-config --cflags --libs grpc++ protobuf) -o grpc_smoke
          ./grpc_smoke
```

- [ ] **Step 2: Validate the YAML**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); print('yaml ok')"`
Expected: `yaml ok`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: GitHub Actions matrix (gcc/clang x protobuf 3.20/3.21) + grpc smoke"
```

---

### Task 8: Doc truth (match code)

Criterion I: kill the "throw" and "tamper" claims in the **live** docs under `grpc-routing-meta/` (refs/ stays untouched). Preserve the scored *benefit* claims; change only mechanism wording.

**Files:**
- Modify: `grpc-routing-meta/CONTEXT.md`
- Modify: `grpc-routing-meta/OVERVIEW.zh.md`
- Modify: `grpc-routing-meta/README.md`

- [ ] **Step 1: `CONTEXT.md` — invariant 6 (integrity, not tamper)**

Replace in invariant 6: `The receiver recomputes and compares; mismatch ⇒ header/body drift or tamper.`
with: `The receiver recomputes and compares; mismatch ⇒ header/body drift (sender bug, sender↔verifier version skew, or transit mangling). Integrity check, not a security control — no key, no signature (SPEC §5.3).`

In the checklist, change `Digest round-trip (6): sender→receiver VerifyDigest ok; tamper ⇒ fail.`
to: `Digest round-trip (6): sender→receiver VerifyDigest ok; modified context ⇒ fail (drift detected).`

- [ ] **Step 2: `CONTEXT.md` — invariant 9 (ProjResult, not throw)**

Replace in invariant 9: `and `required` — `ProjectMeta` throws if the source field is empty.`
with: `` and `required` — if the source is empty, `ProjectMeta` reports a `MissingRequired` issue in its `ProjResult` (`ok=false`) and emits `x-routing-error: missing:<key>`; it does **not** throw and does **not** emit the empty header. ``

In the checklist, change `Domain scalar (9): nested path reached; empty ⇒ throws.`
to: `Domain scalar (9): nested path reached; empty ⇒ ProjResult MissingRequired issue + x-routing-error, no throw.`

- [ ] **Step 3: `CONTEXT.md` — code map + invariant 10**

In the "Header model" table, add `x-routing-error` to the domain-scalar row's headers. In the Code map table, add two rows after the `metadata_sink.h` row:

```
| `example/src/common/proj_result.h` | `ProjResult{ok, issues[], duration}` + `Issue{MissingRequired, Overflow}` — what a projection reports |
| `example/src/common/send.h` | `Send<>()` = `FillCommon` + `ProjectMeta`, returns `ProjResult` |
```

Update the plugin row to: `codegen: emits `ProjectMeta()` returning `ProjResult` (scalar walk + context loop → helper); required-missing → issue + `x-routing-error`, never throws`. Update invariant 10's `Send(...)` line to note it returns `ProjResult`.

- [ ] **Step 4: `OVERVIEW.zh.md` — §4 table + §7 row**

In the §4 table, replace the row cell `required` 的 scalar 沒填 → `ProjectMeta` 當場 **throw**,不會送出空值
with: `required` 的 scalar 沒填 → 回報 `ProjResult{ok=false, MissingRequired}` 並發 `x-routing-error: missing:<key>`(**不 throw**、也不送空值);caller 依 `issues` 決定 abort/proceed

In the §7 「Error handling」 row, replace the 靠什麼 cell `codegen 驗證 + `ProjectMeta` throw + `EmitProcessContexts` + `VerifyDigest``
with: `codegen 驗證 + `ProjectMeta` 回報 `ProjResult`(`x-routing-error`)+ `EmitProcessContexts` 顯式 overflow + `VerifyDigest``

(The §4 narrative line "digest 在 sender 端就算好…在來源就被 capture" stays — it already frames the digest as drift/consistency, not tamper.)

- [ ] **Step 5: `README.md` — error model + bench in run list**

In the "Build & run" block, add after the `test_projection` line:
```sh
./build/bench_projection   # per-call duration for 1/2/25/60 contexts (sub-ms)
```
Add a short "Error model" note near the Size guard section:
```markdown
## Error model (report, don't dictate)

`ProjectMeta`/`Send` return `ProjResult{ok, issues[], duration}` and never throw on a
data condition. A missing **required** scalar (sys3 `x-mask-id`) sets `ok=false`, records
a `MissingRequired` issue, and emits `x-routing-error: missing:x-mask-id` (the empty header
is not sent). Overflow is a non-blocking issue (`ok` stays true). The caller inspects
`issues` and decides; the kit logs nothing.
```

- [ ] **Step 6: Verify no stale claims remain**

Run:
```bash
grep -nE 'throw|tamper' grpc-routing-meta/CONTEXT.md grpc-routing-meta/OVERVIEW.zh.md grpc-routing-meta/README.md || echo "clean"
```
Expected: `clean` (or only matches inside code/identifier context you've intentionally kept — review each hit; there should be none describing the runtime behavior).

- [ ] **Step 7: Commit**

```bash
git add grpc-routing-meta/CONTEXT.md grpc-routing-meta/OVERVIEW.zh.md grpc-routing-meta/README.md
git commit -m "docs: ProjResult/x-routing-error + integrity-only digest (match code)"
```

---

### Task 9: Thread-safety test (cheap P1)

Document and assert re-entrancy: projection holds no shared mutable state, so concurrent calls into separate sinks are safe and deterministic.

**Files:**
- Modify: `grpc-routing-meta/example/tests/test_projection.cc` (add include + block)

- [ ] **Step 1: Write the failing test**

Add `#include <thread>` to `tests/test_projection.cc`, and a block before `ALL TESTS PASSED`:

```cpp
  // --- thread-safety: projection is re-entrant (no shared state) [cheap P1] ---
  {
    auto req = sys1Req(2);
    const int N = 8;
    std::vector<std::thread> ts;
    std::vector<std::string> digests(N);
    std::vector<bool> oks(N, false);
    for (int i = 0; i < N; ++i)
      ts.emplace_back([&, i] {
        routingmeta::VectorSink sink;
        routingmeta::ProjResult r = routingmeta::ProjectMeta(req, sink);
        oks[i] = r.ok;
        digests[i] = sink.Get("x-process-context-digest");
      });
    for (auto& t : ts) t.join();
    for (int i = 0; i < N; ++i) {
      assert(oks[i]);
      assert(!digests[i].empty());
      assert(digests[i] == digests[0]);                          // deterministic across threads
    }
  }
```

- [ ] **Step 2: Ensure the threads library links**

`std::thread` needs `-pthread` on Linux. In `build.sh`, add `-pthread` to the `test_projection` compile line:

```bash
echo "[test] test_projection"
$CXX $CXXFLAGS -pthread tests/test_projection.cc "${GEN_SRCS[@]}" "${PBFLAGS[@]}" -o "$BIN/test_projection"
```

In `CMakeLists.txt`, link Threads to the test target — add near the top after `find_package(Protobuf REQUIRED)`:

```cmake
find_package(Threads REQUIRED)
```
and change the test link line to:

```cmake
target_link_libraries(test_projection PRIVATE routing_meta_gen Threads::Threads)
```

- [ ] **Step 3: Run to verify it passes**

Run: `cd grpc-routing-meta/example && ./build.sh && ./build/test_projection`
Expected: `ALL TESTS PASSED`. (On macOS `-pthread` is accepted and a no-op-ish; on Linux it links pthreads.)

- [ ] **Step 4: Commit**

```bash
git add grpc-routing-meta/example/tests/test_projection.cc grpc-routing-meta/example/build.sh grpc-routing-meta/example/CMakeLists.txt
git commit -m "test: concurrent projection thread-safety"
```

---

## Final verification (BRIEF Verify block)

- [ ] Run the full acceptance sequence:

```bash
cd grpc-routing-meta/example && ./build.sh    # plugin builds, codegen runs, negative gate ok x3, binaries link
./build/unified_sender                         # 3 system blocks; empty sys3 mask -> x-routing-error + duration
./build/receiver_verify                        # digest OK
./build/test_projection                        # ALL TESTS PASSED
./build/bench_projection                       # sub-ms for 1/2/25/60 contexts
```

- [ ] Confirm criteria A–I against `refs/BRIEF.md`; CI authored for the Linux × {gcc,clang} × {protobuf 3.20,3.21} matrix.
- [ ] Request code review (superpowers:requesting-code-review).

## Self-review notes (coverage map)

- **A** Portable build → Task 5 (build.sh) + existing `find_package` CMake; bench/threads wiring Tasks 6, 9.
- **B** CI matrix → Task 7.
- **C** No silent failure → build gate (already present, re-verified Task 2 step 5) + sender `x-routing-error` (Tasks 2, 4) + overflow issue (Tasks 1, 2) + receiver digest (existing).
- **D** Exact projection → existing asserts retained (Task 2 keeps encoding/key-sort/digest); round-trip in `receiver_verify`.
- **E** One sender path → Task 3 (`Send` in kit), Task 4 (sender uses it).
- **F** Policy centralized → unchanged `process_context_emit.h` constants (Tasks 1).
- **G** Testable invariants → `test_projection` updates (Tasks 1–3, 9); negative gate in CI (Task 7).
- **H** Perf observed → `duration` (Task 2) + bench (Task 6).
- **I** Docs match code → Task 8.
