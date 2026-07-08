# handoff — grpc-routing-meta：先學、再改 proto、build、test

接手對象：agent 或人，兩者照同一份流程手動執行都要能通。
工作目錄：`grpc-routing-meta/example/`（本 workspace 內，勿外出）。

## 0. 規則（先讀，違反會白做）

- `CLAUDE.md`：commit **不加 Co-Authored-By**、訊息簡潔具體、**只 commit 到 local 不 push**、
  不讀 workspace 外的東西、不讀已刪除的 `archive/` 舊設計。
- 設計依據只有兩個：`refs/`（SPEC、CONTEXT、OVERVIEW、BRIEF、plan）+ live code。
- 每次改動的綠燈標準：`./build.sh` 全綠（negative codegen gate + test_projection + receiver_verify）。

## 1. 現況（2026-07-09，branch `productionize-grpc-routing-meta`）

一切綠。這輪 session 完成的事（細節看 `git log --oneline -15`）：

- sys2 = demo 裡的 RMS，含四個 sender pattern（tool+recipe / 空 recipe 錯誤 / FOUP 多 lot /
  真實形狀 `rqst_RMS_GetRecipeSet`），全部有測試釘住。
- `TUTORIAL.zh.md`（repo 根）：完整教學——§1.1 舊 toolchain 相容性、§2 先跑 sys2 +
  §2.1 五個實跑案例（console 形式）、§3 一步步加新系統、§4 sender、§5 receiver、§6 驗收清單。
- 修過一個 codegen bug：plugin 現在用 `lowercase_name()` 產 getter（camelCase 真實 proto
  才編得過），由 sys2 real-shape 測試區塊釘住。

## 2. 你的任務流程（照順序）

### Step 1 — 教學（不要跳過，約 15 分鐘）

1. 讀 `grpc-routing-meta/TUTORIAL.zh.md` §0–§2。
2. 手動跑一次，確認環境是好的：

```bash
cd grpc-routing-meta/example
./build.sh                    # 預期最後一行：OK -> binaries in .../build/
./build/unified_sender        # 預期五個 === sys2 區塊：四個對照 TUTORIAL §2.1 案例 1–4，加 List (count=0)
./build/receiver_verify       # 預期最後一行：result: PASS (clean accepted, tampered rejected)
```

3. 對照 §2.1 的五個案例讀懂 in/out（sender 填什麼 → wire 出什麼 → receiver 驗什麼）。

### Step 2 — 改 proto（照 TUTORIAL §3，一步一驗）

拿到要接的真實 proto 後（或用 §3 的 nrms 例子練習）：

1. `cp <你的>.proto proto/`，然後 `build.sh` 的 `SYSTEMS=(sys1 sys2 sys3)` 加上你的 stem，
   `CMakeLists.txt` 的 `foreach(name sys1 sys2 sys3)` 同步加。
2. 按 §3.1 決策表下 tag（口訣：sender 自己就知道 → 不 tag 走 Runtime；body 單值 →
   `(routing.project)`；body 多值 → `(routing.pctx)`）。成品長相對照 `proto/sys2.proto`
   的 `rqst_RMS_GetRecipeSet`。
3. 常見雷（本 session 踩過/釘過的）：
   - camelCase 欄位 OK，但 C++ getter 是小寫（`recipeId` → `set_recipeid()`）。
   - 一個 request 只能有一個 pctx-bearing 的 repeated 欄位。
   - project tag 不能在 repeated 子樹下；batch 同值才用 `uniform_across_repeated`。
   - tag 錯 codegen 會 fail loud，錯誤訊息對照表在 §3.3。

### Step 3 — Build

```bash
./build.sh    # 全綠才算過；紅了看 §3.3 錯誤表，改 proto 再跑
```

### Step 4 — Test（兩件事都要做）

1. **加測試**：抄 `tests/test_projection.cc` 的「sys2 real-shape」區塊，改成你的 message
   名和期望 header 值（`./build.sh` 會自動跑它）。
2. **手動驗**：在 `sender/unified_sender.cc` 照 pattern 3 加一個 demo block 用
   `VectorSink` dump，跑 `./build/unified_sender` 逐行核對 §5 的 header 表。
3. 完成標準 = TUTORIAL §6 驗收清單（含空 required 欄位 → `x-routing-error`、
   25/26 lot 的 overflow 行為）。

每完成一步 commit 一次（規則見 §0）。

## 3. 快速參考

| 要查什麼 | 在哪 |
|---|---|
| tag 語義 / 決策 | `proto/metadata_options.proto` 註解 + TUTORIAL §3.1 |
| header 合約（哪些恆出、何時 digest） | TUTORIAL §5 表、`refs/SPEC.md` §2/§5 |
| codegen 拒絕訊息 | TUTORIAL §3.3 表；完整清單在 `build.sh` 的 negative gate case 區 |
| 舊 toolchain（protobuf/gRPC/C++ 底線、proto3 optional） | TUTORIAL §1.1 |
| sender 接法（GrpcSink 編譯條件、source_system 是呼叫端身分） | TUTORIAL §4 |
| receiver 驗法（VerifyDigest verify-if-present、ParseContext） | TUTORIAL §5 + `receiver/receiver_verify.cc` |

## 4. 工作方法（任何 agent / 人皆適用；環境有對應 skill 就調用，沒有就照文字做）

- **測試先行**（Claude Code skill：`superpowers:test-driven-development`）——Step 2 加新系統時，
  先在 test_projection 寫下期望的 header 值再下 tag，讓紅→綠告訴你 tag 對了沒。
- **端到端實跑驗證**（skill：`verify`）——每個 step 完成後跑 binary 看實際輸出，
  不是只看編譯過；§2 Step 1/4 的指令和預期輸出就是驗證腳本。
- **紅燈系統化排查**（skill：`superpowers:systematic-debugging`）——build.sh 紅燈先查
  TUTORIAL §3.3 錯誤表；查不到再從 plugin 的錯誤字串反查 `src/plugin/protoc-gen-meta.cc`，
  不要猜著改。
- **commit 前自我 review**（skill：`code-review`）——對 diff 檢查：是否最小改動、
  是否有測試釘住、commit 訊息是否符合 §0 規則。

## 5. 未做 / 已知開口（接手可選）

- receiver 端未檢查 `x-process-context-format` 值（目前 wire 上只寫不讀）；若要補：
  在 `ParseContext` 前比對 `urlencoded-query-string-v1`，不認識即拒收 + 測試。
- TUTORIAL §5 可補「legacy sender 相容守則」：kit 檢查全部 gate 在 kit headers 在場
  （判別鍵：`x-process-context-count` 恆出），不在場當 legacy 放行。
