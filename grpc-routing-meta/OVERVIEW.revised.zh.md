# gRPC Routing Metadata Kit — 設計總覽(修正版)

> **Body 是唯一真相,header 是 body 的「投影」,而這個投影由 codegen 自動產生。**
> Sender 端只多兩行、不分系統;Provider 在既有 proto 上貼一個標籤;錯誤在 build /
> sender 端就被擋下 —— 但**不是用 throw,而是回報結構化結果**,由 sender 自行決定。

> 修正版說明:本檔對齊**實際實作**。與舊版差異:失敗模型由 throw 改為
> `ProjResult`(report, don't dictate);digest 定性為 **integrity, not security**;
> `Send` 釐清為 **sender 部門擁有**(kit 只出 primitives + sample);overflow 門檻寫精確。
> 規範以 [`SPEC.md`](SPEC.md) 為準,衝突時 SPEC 勝。

---

## 1. 目的

- APISIX 要靠 **header 做 content routing**,但不該、也不想去 parse protobuf body。
- header 必須是 body 的**精確投影**,不能各系統各寫一套、悄悄漂移。
- 橫跨 **sys1 / sys2 / sys3 三系統、共 ~21 種 transaction**(本 example 示範其中 16:1 / 5 / 10),
  而 sender 端的接線只有**一份**。
- 範圍:**全 C++、單一共用 kit**(ADR 0001)。byte 規則本身與語言無關,但本 kit 不出
  跨語言 spec 或 conformance vectors —— 若未來真有非 C++ sender,再補。

---

## 2. 簡單在哪

### 2.1 Sender —— 在呼叫前多兩行,不分系統

```cpp
#include "common/common_headers.h"   // Runtime + FillCommon(6 個 common header)
#include "sys1.proj.h"                // 你本來就會 include 的 request 型別 + 生成的 ProjectMeta

// ── 你本來就會寫的 ──
sys1::v1::CalculateRequest req;  /* ...填 body... */   grpc::ClientContext ctx;

// ── 只多這兩行(building blocks,kit 保證) ──
routingmeta::GrpcSink sink(&ctx);                       // 1) 包一下 context
FillCommon(rt, sink);                                   // 2a) 共同 header
routingmeta::ProjResult r = ProjectMeta(req, sink);     // 2b) body 投影 → 結構化結果

// r.ok / r.issues 你自己決定 abort 或 proceed(kit 不替你決定,也不 log)
stub->Calculate(&ctx, req, &resp);
```

`ProjectMeta` 依 **request 型別**自動挑對應的 overload(由 sink 引數的 ADL 解析),所以
sys1 / sys2 / sys3 的 transaction **全走同一條路、沒有任何 `if (system==…)`**。

**關於 `Send`:** 把這兩行包成一個 `Send(req, rt, sink)` 是**選用**的便利包裝,而且
**屬於 sender 部門**(那是 orchestration:何時呼叫、怎麼組 `Runtime`、拿到
`ProjResult` 後 abort/proceed/log/metric)。kit 的 authoritative 介面只有
`FillCommon` + `ProjectMeta` + `ProjResult` + `MetadataSink`;example sender 裡的
`Send` 只是**示範接線**,不是 kit 的 API。**Wiring 契約**:先 `FillCommon` 再
`ProjectMeta`、同一個 sink;讀 `ProjResult` 自行決策。

### 2.2 Provider —— 在既有 proto 上 annotate,搭原本的 protoc 流程

不必另開工具、不必手寫抽取邏輯。原本就在跑 `protoc --cpp_out`;我們**只多掛一個
`--meta_out`**,同一支 proto 順手把投影邏輯也生出來:

| | 傳統 `--cpp_out` | 我們的 `--meta_out` |
|---|---|---|
| **機制** | protoc plugin / descriptor → 程式碼 | 一模一樣 |
| **產出** | message class(getter/setter、序列化) | `ProjectMeta()`:body → routing metadata 的投影 |
| **角色** | 定義資料型別 | 讀那些生成的型別,投影成 header |

```
   sys1.proto / sys2.proto / sys3.proto   (+ annotation)
                     │  protoc
         ┌───────────┴───────────┐
     --cpp_out               --meta_out
         │                       │
      *.pb.*                  *.proj.*
   message class            ProjectMeta()
  (getter / setter)       (body ─投影─▶ header)
         └──── proj 呼叫 pb 的 getter ────┘
```

Provider 三件事:`import` 兩支共用合約 → 加一個 `repeated ProcessContext contexts`
→(若有 domain 專屬值)在欄位上貼一個 `(routing.project)` 標籤。合約共用,所以三系統
schema 不會分歧。

---

## 3. 三個系統實際收到的 metadata

6 個 common header 三系統**一字不差**(只值不同),以下省略,只列各自的 Layer 3。
**真實輸出**(取自 `unified_sender`):

```
共同 :  x-request-id · x-correlation-id · x-contract-version
        x-source-system · x-site-id · x-tool-id

sys1  Calculate ── 批次 2 筆,欄位全填
  x-process-context-count:   2
  x-process-context-format:  urlencoded-query-string-v1
  x-process-context-digest:  sha256:efafba16…
  x-process-context: ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=PART-A&RecipeID=RCP_ETCH_V3&StageID=ETCH&Tech=N5
  x-process-context: ChamberId=CH-B&LotID=LOT02&OperationNO=OP100&PartID=PART-A&RecipeID=RCP_ETCH_V3&StageID=ETCH&Tech=N5

sys2  Verify ── 共用 schema,現場常只有 RecipeID(其餘空)
  x-process-context-count:   1
  x-process-context-format:  urlencoded-query-string-v1
  x-process-context-digest:  sha256:6526d80d…
  x-process-context: ChamberId=&LotID=&OperationNO=&PartID=&RecipeID=RCP_ETCH_V3&StageID=&Tech=

sys2  List ── 沒有 context
  x-process-context-count:   0
  x-process-context-format:  urlencoded-query-string-v1

sys3  Submit05 ── domain scalar,無 context
  x-mask-id:                 RET-9981
  x-process-context-count:   0
  x-process-context-format:  urlencoded-query-string-v1

sys3  Submit05(mask 為空)── 失敗即旗標,不送空值、不 throw
  x-routing-error:           missing:x-mask-id
  x-process-context-count:   0
  x-process-context-format:  urlencoded-query-string-v1
  → r.ok=false, issues=[MissingRequired "x-mask-id"]
```

重點:**有就帶、沒有就 `count=0` 或空值(`Key=`)**,永遠是 body 的忠實投影。
`count` 與 `format` **永遠送**(描述 body,即使 context 被抑制);`digest` 與 context
行**只在 context 非空且未 overflow 時**才出;**digest 本身可選**(預設開、可關 —— 見 §4)。

---

## 4. Error control —— 錯誤在「來源」就被擋住,且**回報而非獨斷**(report, don't dictate)

三道關卡,全部在送上 wire 之前:

| 時機 | 擋什麼 | 怎麼擋 |
|---|---|---|
| **Build(codegen)** | annotation 寫錯:key 重複、`(routing.project)` 放在 repeated / message 底下 | `protoc --meta_out` **直接失敗**,並回報**確切原因** |
| **Run(sender)** | `required` 的 scalar 沒填(如 sys3 `x-mask-id`) | **不 throw**:回 `ProjResult{ok=false, issues:[MissingRequired]}` + emit `x-routing-error: missing:<key>`,並**抑制**那個空 header |
| **Run(sender)** | metadata 過大會被 APISIX/HTTP2 默默截斷 | total>**7168B** OR count>**25** OR 單行>**512B** → 改發**顯式** `x-process-context-overflow: true`(**非阻斷**:請求照走,記 `Issue{Overflow}`,`ok` 不變) |
| **Receiver** | header / body 漂移 | 用 `x-process-context-digest` **重算比對**,不符即 reject(**digest 可選**:關閉時 sender 不產生、receiver **有才驗**,缺席即跳過) |

**失敗模型 = report, don't dictate(SPEC §7)。** kit 對資料條件**永不 throw、永不 log**;
它只回 `ProjResult{ ok, issues[], duration }`。abort 還是 proceed、要不要 log/metric,
是 **sender(caller)的決定**,不是 kit 的。`x-routing-error` 的預設處置仍是
**跨部門待決事項**(Sender dept ↔ 平台)。

**digest 是 integrity,不是 security(ADR 0002)。** `x-process-context-digest =
"sha256:" + sha256(契約字串以 '\n' join)`,sender 端算、receiver 端重算比對 —— 抓的是
**意外漂移**(sender bug、版本 skew、傳輸破壞)。它**無 key、無簽章**:能改 body 的人
可一併重算 digest,所以**不**防惡意竄改。下游不可把它當防竄改封條。

**digest 可選(預設開啟)。** 某些 deployment 可關閉:關閉時 sender **不 emit** digest、
receiver 採「**有才驗**」—— digest 缺席就跳過驗證、**不**當作 drift。(開關集中一處;
context 行、count、format、overflow 都不受影響。)

---

## 5. 整體流程(一張圖)

```
 build │  proto (+1 個 tag) ──protoc──▶  message class  +  ProjectMeta()
───────┼──────────────────────────────────────────────────────────────
 run   │  sender:  FillCommon(共同 6 header)
       │           ProjResult r = ProjectMeta(body ──投影──▶ header)
       │           r.ok / r.issues → sender 自行決策(abort/proceed/log)
───────┼──────────────────────────────────────────────────────────────
 edge  │  APISIX   只看 header 路由,不 parse body
 back  │  sys1 / sys2 / sys3   重算 digest 驗證一致
```

---

## 6. E2E 作法(誰、做什麼、什麼順序)

```
① 平台 / API provider ── 一次性
     • protoc-gen-meta plugin(codegen 投影邏輯)
     • 共用合約:metadata_options.proto(標籤)+ process_context.proto(7 欄 schema)
     • 政策常數集中一處(7168 / 25 / 512、HPACK +32)
② 各系統 sys1 / sys2 / sys3 ── 各自獨立
     • import 兩支合約 + 加 repeated ProcessContext(+ 有 domain scalar 才貼 (routing.project))
     • 不寫任何抽取程式碼
③ build + 驗證 ── 同一條 protoc 流水線
     • --cpp_out + --meta_out 一起跑;codegen 階段就驗 annotation(錯就直接失敗)
     • 跑 test / receiver:digest round-trip、required→ProjResult、size guard、negative-codegen 綠燈才算過
④ sender 套用 ── 每個用戶端(sender 部門)
     • include kit 的 building blocks;接 FillCommon + ProjectMeta(可自包成 Send)
     • 讀 ProjResult 決定 abort/proceed;不分系統、不動既有 RPC 呼叫
⑤ 上線 / test
     • metadata 自動帶上;sender 端就能 verify(錯誤當場 capture 成資料)
     • APISIX 看 header 路由;backend 重算 digest 再確認
```

成本結構:**平台做一次 plugin、系統各貼一個標籤、sender 接兩行**——
`O(1) plugin + O(系統) 一個 annotation + O(用戶端) 兩行`,**沒有** `O(系統 × header)`
的手寫抽取。新增第 4 個系統:加 1 支 proto + build list 一行,plugin / 合約全部不動。

---

## 7. 還有什麼好處(對齊實作)

| 面向 | 好處 | 靠什麼 |
|---|---|---|
| **Effort 省力** | provider 一個 annotation、sender 接兩行;新增系統 = 1 proto + build list 一行 | codegen 自動化「值埋在哪」,不用每系統手寫抽取 |
| **治理 / 文件** | 標籤 + 共用合約 = 唯一、可執行的定義 | annotation + 共用合約 |
| **時程 / rollout** | 改 proto 重 codegen,全員自動跟上 | codegen + 共用 lib |
| **Error handling** | build 擋 annotation;sender `required` → `ProjResult` + `x-routing-error`(**不 throw**);7 KB overflow **顯式且非阻斷**;receiver digest;**不會 silent failure** | codegen 驗證 + ProjResult + EmitProcessContexts + VerifyDigest |
| **Report, don't dictate** | kit 不 throw、不 log、不替你決定 abort/proceed;只回結構化結果 | `ProjResult{ok, issues[], duration}` |
| **Quality / 一致性** | header 保證是 body 投影、不漂移 | 單一 source of truth(body)+ canonical encoding + sha256 digest(integrity) |
| **不分歧** | 三系統共用同一份 schema;sender 一條路、無 copy-paste | 共用 `process_context.proto` + 單一 ADL 解析 |
| **好維護** | 政策(7168 / 25 / 512 / +32)集中一處;一支 plugin 管全部 | `process_context_emit.h` + 單一 plugin |
| **解耦** | kit 不綁 gRPC;routing policy 在 gateway,不寫死進 contract;**orchestration 留給 sender** | `MetadataSink` 抽象 + primitives-only 介面 |
| **可觀測** | kit 自計時,caller 讀 `duration`(bench 實測 sub-ms) | `ProjResult.duration`(kit 不內建 logger) |
| **可測** | invariant 可逐條 assert;codegen 有負面測試(重複 key / repeated / message 被擋,且驗確切原因) | `CONTEXT.md` invariants + `tests/negative/` |

一句話:**把「容易出錯、會漂移」的 header 投影,變成編譯期自動產生、來源端自我驗證、
且以結構化結果回報的東西** —— 省力、難錯、好維護,而且 kit 不越界替 sender 做決定。

> 已移除的舊賣點:「跨語言可逐字節重現」。本 kit 決策為全 C++(ADR 0001),不出跨語言
> 合約;byte 規則雖與語言無關,但那不是目前支援的承諾。

---

## 8. Hardcode vs Autogen(代碼量)

投影邏輯(抽值 / 排序 / encode / digest / overflow / nested mask 路徑)**整段由 codegen
產、人寫 0 行**;sender lane 完全不碰。Hardcode 世界裡這段是每個 sender、每種語言、每個
transaction 各手寫一次。

| | Hardcode | Autogen(本套件) |
|---|---|---|
| Provider 每系統 | proto(無標籤) | proto + ~5 行標籤 |
| 投影 / encode / digest / overflow | 每個 sender 手寫 | **codegen 產(人寫 0 行)** |
| Sender 每次呼叫 | 每 tx 手寫抽取 + 設 header | **接兩行** |
| body 改了 | 每處手動同步,漏一處即 silent drift | 重 codegen,全員自動跟上 |
| 巢狀值(如 mask) | 每 tx 各手寫一條巢狀路徑 parse body | `walkProj` 自動產生 getter 路徑 |

重點不是「autogen 行數少」(一次性平台 src ≈ plugin ~210 + helper ~250 + 合約 ~27,不含註解),而是 **投影 0 人寫、邊際 ≈ 0**;
hardcode 是每 sender × 每語言數百行、且要一直維護。sender / 改動越多,差距越大。

---

## 9. 時程與工時(man-day,**粗估**)

> 以下為 rough sizing(假設 3 系統、~2 sender、21 tx),**估算值、非程式事實**,
> 用來看「成本落在哪、會不會攤平」。

| 階段 | Autogen | Hardcode | 說明 |
|---|---:|---:|---|
| ① API plugin dev | ~10(一次性) | ~3 | autogen 做 plugin + helper + 合約 |
| ② 3 sys 改 proto | ~1.5 | ~1 | autogen 多貼標籤 |
| ③ APISIX 改 | ~0.5 | ~0.5 | 兩邊基本沒改 |
| ④ sender 改 code | ~2 | **~18 ⚠** | autogen 接兩行(與 tx 數無關);hardcode 每 sender 手寫 21 種投影 |
| ⑤ 整合測試 | ~2.5 | **~9** | hardcode 易錯,debug digest / 編碼 / 路徑 |
| **首次 rollout** | **~16.5** | **~31.5** | |

要點:autogen 把成本前置到一次性 plugin,之後**邊際 ≈ 0**;hardcode 的 ④ sender 改 code
是大頭,且每新增 sender / 改動都重來、不攤平。

---

## 10. 總比較表(多面向 + 優 / 平 / 輸)

> 用我們的**實際情境**評:只有 1 個 sender、看首次 rollout、先不考慮後續改動 ——
> 所以 autogen「跨 sender 攤平 / 改動自動 rollout」這類長處在這情境先不算分。

分三個大面向,由重到輕:**① 品質 / 易出錯**(最重要)→ **② 時程 / Efforts** →
**③ 延展性**(本情境較不重要)。

| 面向 | Hardcode(手寫) | Autogen(本套件) | 對 autogen |
|---|---|---|:--:|
| **① 品質 / 易出錯(最重要)** | | | |
| &nbsp;&nbsp;規格 / 文件 | 沒定義、沒文件,各自猜 | 標籤 + 合約 = 唯一、可執行的定義 | 優 |
| &nbsp;&nbsp;一致性 / 漂移 | 各自實作、無強制 → 易漂移 | 產 body 時一併投影到 header → **強制一致** | 優 |
| &nbsp;&nbsp;巢狀值抽取(mask) | 每個 tx 各自手寫巢狀路徑硬抽(像 XPath),欄位一改就漏 | `walkProj` 依 proto 自動生 getter 路徑 | 優 |
| &nbsp;&nbsp;錯誤處理(silent failure) | metadata 爆量 / 漏欄位 → APISIX 直接 reset / truncate,silent、難查 | build / sender / receiver 三關 + 7 KB 顯式 overflow + **`ProjResult` 結構化回報**,絕不 silent | 優 |
| &nbsp;&nbsp;測試 | 手寫易錯、debug 重 | invariants 可測 + codegen 負面測試(驗確切原因) | 優 |
| &nbsp;&nbsp;Sender 每次呼叫 | 每 tx 手寫投影 | 接兩行 | 優(輕微)³ |
| **② 時程 / Efforts** | | | |
| &nbsp;&nbsp;工時 + 時程(21 tx 首次) | ~31.5 md,④ sender 改 code 拖最久 | ~16.5 md,改 proto 自動 rollout | 優 |
| &nbsp;&nbsp;Provider 每系統 | 定義 proto | proto + ~5 行標籤 | 平(多 5 行) |
| &nbsp;&nbsp;代碼量(人寫) | 一個 sender ~250–400 行 | 投影 0 人寫;一次性 src ≈ plugin ~210 + helper ~250 + 合約 ~27(不含註解) | 輸 ¹ |
| **③ 延展性(本情境重要性較低)** | | | |
| &nbsp;&nbsp;改動 / 維護 + 新增系統 | 每處手動同步、重來 sender + test | 改 proto 重 codegen(+1 proto) | 平 ² |

¹ 別被 ~500 騙:其中 helper ~250 是 hardcode 自己也要寫的 mechanism。autogen 真正多付的只有
**plugin ~210**,換來投影永遠 0 人寫。所以只有「1 sender、首次」會輸;第 2 個 sender 起就贏。
² 新增 / 改動時**兩邊都仍要動一處**:hardcode 改 sender + test,autogen 改 proto 重 codegen
—— autogen 只改一處且自動 rollout,但本情境(1 sender、不計後續)先算平;**有第 2 sender 或
開始改動即轉優**。
³ 與「巢狀值 / 代碼量」同源,每次呼叫的絕對差距小,不另列為大勝。

> 結論:嚴格只看「**1 sender、首次、不計後續**」,autogen 是 **7 優 / 2 平 / 1 輸**
> —— 唯一輸的是「代碼量」(一次性 src 較多)。把後續改動 / 多 sender 算進來,2 個平也轉優。

---

**一句總結:Provider 貼一個標籤、走原本的 codegen;Sender 接兩行(`FillCommon` +
`ProjectMeta`,要不要包成 Send 是 sender 自己的事);header 保證是 body 的投影,錯了在
來源就被擋下並回報成資料 —— 不 throw、不 silent、不替你決定。**

> 規範細節見 [`SPEC.md`](SPEC.md);可測 invariants 見 [`CONTEXT.md`](CONTEXT.md);
> 設計取捨見 [`docs/adr/`](docs/adr/);可跑的程式在 [`example/`](example/)。
