# gRPC Routing Metadata Kit — 設計總覽

> **Body 是唯一真相,header 是 body 的「投影」,而這個投影由 codegen 自動產生。**
> Sender 用起來兩行、不分系統;Provider 只在既有 proto 上貼一個標籤;錯誤在
> build / sender 端就被擋下,不會 silent failure。

---

## 1. 目的

- APISIX 要靠 **header 做 content routing**,但不該、也不想去 parse protobuf body。
- header 必須是 body 的**精確投影**,不能各系統各寫一套、悄悄漂移。
- 橫跨 **sys1 / sys2 / sys3 三系統、共 ~21 種 transaction**(本 example 示範其中 16:1 / 5 / 10),但 sender 程式只有**一份**。

---

## 2. 簡單在哪

### 2.1 Sender —— include 一支 header,每次呼叫就兩行,不分系統

```cpp
#include "common/common_headers.h"      // Runtime + FillCommon
#include "sys1.proj.h"                    // 你本來就會 include 的 request 型別

// ── 你本來就會寫的 ──
sys1::v1::CalculateRequest req;  /* ...填 body... */   grpc::ClientContext ctx;

// ── 只多這兩行 ──
routingmeta::GrpcSink sink(&ctx);                     // 1) 包一下 context
FillCommon(rt, sink);  ProjectMeta(req, sink);        // 2) 共同 header + body 投影

stub->Calculate(&ctx, req, &resp);                    // 你本來就要呼叫
```

`ProjectMeta` 依 **request 型別**自動挑對應的 overload,所以 sys1 / sys2 / sys3 的 21 種
transaction **全走同一條路、沒有任何 `if (system==…)`**。換系統、加 method,sender 一行都不用改。

### 2.2 Provider —— 在既有 proto 上 annotate,搭原本的 protoc 流程

不必另開工具、不必手寫抽取邏輯。原本 grpc 就在跑 `protoc --cpp_out`;我們**只多掛一個
`--meta_out`**,同一支 proto 就順手把投影邏輯也生出來:

| | 傳統 `--cpp_out` | 我們的 `--meta_out` |
|---|---|---|
| **機制** | protoc plugin / descriptor → 程式碼 | 一模一樣 |
| **產出** | message class(getter/setter、序列化) | `ProjectMeta()`:body → routing metadata 的投影 |
| **角色** | 定義資料型別 | 讀那些生成的型別,投影成 header |

兩者**互補、一起跑**:同一支 proto 先 `--cpp_out` 生出 message class,再 `--meta_out`
生出 `ProjectMeta()`,後者呼叫前者的 getter(`req.contexts()`、`e.chamber_id()`…):

```
   sys1.proto / sys2.proto / sys3.proto   (+ annotation)
                     │
                  protoc
                     │
         ┌───────────┴───────────┐
     --cpp_out               --meta_out
         │                       │
      *.pb.*                  *.proj.*
   message class            ProjectMeta()
  (getter / setter)       (body ─投影─▶ header)
         └──── proj 呼叫 pb 的 getter ────┘
```

Provider 要做的就三件事:`import` 兩支共用合約 → 加一個 `repeated ProcessContext
contexts` →(若有 domain 專屬值)在欄位上貼一個 `(routing.project)` 標籤。合約是共用的,
所以三系統的 schema 不會分歧:

```
metadata_options.proto   標籤定義     ┐ shared
process_context.proto    共用 7 欄 schema ┘  (平台 / 合約方擁有)
        ▲ import
sys1.proto / sys2.proto / sys3.proto    各系統自己的 request(貼標籤)
```

---

## 3. 三個系統實際收到的 metadata

6 個 common header 三系統**一字不差**(只有值不同),以下省略,只列各自的 Layer 3:

```
共同 :  x-request-id · x-correlation-id · x-contract-version
        x-source-system · x-site-id · x-tool-id

sys1  sys1.control.calculate ── 批次 2 筆,欄位全填
  x-process-context-count:  2
  x-process-context-digest: sha256:efafba16…
  x-process-context: ChamberId=CH-A&LotID=LOT01&OperationNO=OP100&PartID=PART-A&RecipeID=RCP_ETCH_V3&StageID=ETCH&Tech=N5
  x-process-context: ChamberId=CH-B&LotID=LOT02&OperationNO=OP100&PartID=PART-A&RecipeID=RCP_ETCH_V3&StageID=ETCH&Tech=N5

sys2  sys2.recipe.verify ── 共用 schema,現場常只有 RecipeID(其餘空)
  x-process-context-count:  1
  x-process-context-digest: sha256:6526d80d…
  x-process-context: ChamberId=&LotID=&OperationNO=&PartID=&RecipeID=RCP_ETCH_V3&StageID=&Tech=

sys2  sys2.recipe.list ── 沒有 context,骨架仍在
  x-process-context-count:  0

sys3 sys3.layout.submit ── domain scalar + 空骨架
  x-mask-id:                RET-9981
  x-process-context-count:  0
```

重點:**有就帶、沒有就 `count=0` 或空值(`Key=`)**,永遠是 body 的忠實投影,不會漂移。

---

## 4. Error control —— 錯誤在「來源」就擋住,不會 silent failure

三道關卡,**全部在送上 wire 之前**:

| 時機 | 擋什麼 | 怎麼擋 |
|---|---|---|
| **Build(codegen)** | annotation 寫錯:key 重複、`(routing.project)` 放在 repeated 底下 | `protoc --meta_out` **直接失敗、生不出來** |
| **Run(sender)** | `required` 的 scalar 沒填 | 回報 `ProjResult{ok=false, MissingRequired}` 並發 `x-routing-error: missing:<key>`(**不 throw**、不送空值);caller 依 `issues` 決定 |
| **Run(sender)** | metadata 太大(>7 KB)會被 APISIX/HTTP2 默默截斷 | 改發**顯式** `x-process-context-overflow: true` |
| **Receiver** | header / body 漂移 | 用 `x-process-context-digest` **重算比對**,不符即 reject |

digest 在 **sender 端**就算好、投影也在 sender 端產生 —— 任何不一致**在來源就被 capture**,
不會跑到 gateway 或 backend 才默默壞掉。把「傳輸層悶掉」變成「應用層看得到的旗標」。

---

## 5. 整體流程(一張圖)

```
 build │  proto (+1 個 tag) ──protoc──▶  message class  +  ProjectMeta()
───────┼──────────────────────────────────────────────────────────────
 run   │  sender:  FillCommon(共同 6 header)
       │           ProjectMeta(body ──投影──▶ header)
       │                          │  gRPC metadata
───────┼──────────────────────────────────────────────────────────────
 edge  │  APISIX   只看 header 路由,不 parse body
       │                          │
 back  │  sys1 / sys2 / sys3   重算 digest 驗證一致
```

---

## 6. E2E 作法(誰、做什麼、什麼順序)

```
① 平台 / API provider ── 一次性,做一次
     • protoc-gen-meta plugin(codegen 投影邏輯)
     • 共用合約:metadata_options.proto(標籤)+ process_context.proto(7 欄 schema)
          │  發佈給所有系統
② 各系統 sys1 / sys2 / sys3 ── 各自、獨立
     • 在自己的 .proto:import 兩支合約 + 加 repeated ProcessContext
       +(有 domain scalar 才)貼一個 (routing.project)
     • 不寫任何抽取程式碼
          │
③ build + 驗證 ── 同一條 protoc 流水線
     • protoc --cpp_out(message class) + --meta_out(ProjectMeta) 一起跑
     • codegen 階段就驗 annotation(重複 key / project 放 repeated 底下 → 直接失敗)
     • 跑 receiver / test:digest round-trip、required 缺值發 x-routing-error、size guard 綠燈才算過
          │  產出可連結的 lib
④ sender 套用 ── 每個用戶端
     • include 一支 header
     • 兩行:FillCommon(共同 header) + ProjectMeta(body 投影)(或包成 Send)
     • 不分系統、不動既有 RPC 呼叫
          │
⑤ 上線 / test
     • 送出時 metadata 自動帶上;sender 端就能 verify(錯誤當場 capture)
     • APISIX 看 header 路由;backend 重算 digest 再確認一次
```

成本結構:**平台做一次 plugin、系統各貼一個標籤、sender 兩行**。是
`O(1) plugin + O(系統) 一個 annotation + O(用戶端) 兩行` ——
**沒有** `O(系統 × header)` 的手寫抽取。新增第 4 個系統:加 1 支 proto +
build list 一行,plugin / sender / 合約全部不動。

---

## 7. 還有什麼好處

| 面向 | 好處 | 靠什麼 |
|---|---|---|
| **Effort 省力** | provider 一個 annotation、sender 兩行;新增系統 = 1 proto + build list 一行 | codegen 把「值埋在哪」自動化,不用每個系統手寫抽取 |
| **治理 / 文件** | 不再「沒明確定義、沒文件、各 sender 各自猜」;標籤 + 合約就是唯一、可執行的定義 | annotation + 共用合約 = 單一 source of definition |
| **時程** | sender 不用為每次改動重寫;改 proto 重 codegen 全員自動跟上,rollout 快 | codegen + 共用 lib |
| **Error handling** | 三道關卡全在來源:build 擋 annotation 錯、sender `required` 缺值發 `x-routing-error` + 7KB overflow、receiver digest;**不會 silent failure** | codegen 驗證 + `ProjectMeta` 回報 `ProjResult`(`x-routing-error`)+ `EmitProcessContexts` + `VerifyDigest` |
| **Quality / 一致性** | header 保證是 body 的投影、不漂移;跨語言可逐字節重現 | 單一 source of truth(body)+ canonical encoding + sha256 digest |
| **不分歧** | 三系統共用同一份 schema;sender 一份程式碼、無 copy-paste bug | 共用 `process_context.proto` + 單一 `Send<>()` |
| **好維護** | 政策(7KB / 25 / 512)集中一處;一支 plugin 管全部;proto 改了 codegen 自動跟上 | `process_context_emit.h` + 單一 plugin |
| **解耦** | kit 不綁 gRPC;routing policy 在 gateway,不寫死進 contract | `MetadataSink` 抽象 |
| **好上手** | `[app]` vs `[+meta]` 逐行標註,採用成本透明、不嚇人 | 程式碼自帶註解 |
| **可測** | 10 條 invariant 可逐條 assert;codegen 有負面測試(重複 key / repeated 被擋) | `CONTEXT.md` invariants |

一句話:**把「容易出錯、會漂移、跨語言難對齊」的 header 投影,變成編譯期自動產生、來源端自我驗證的東西** —— 省力、難錯、好維護。

---

## 8. Hardcode vs Autogen —— 兩條 lane + 代碼量

同一件事(3 系統、~21 transaction)分成 provider 與 sender 兩條 lane:

```
        PROVIDER lane                                   SENDER lane
  ───────────────────────────────              ───────────────────────────────
  [一次性] plugin + 共用合約
  (metadata_options / process_context)
        │
  各系統貼標籤(~5 行/系統)
        │  protoc --cpp_out + --meta_out
        ▼                                              link 產出的 lib
  message class + ProjectMeta()  ───────────▶  include 1 支 header + 寫 2 行
  (投影邏輯整段在這條 lane 自動生成)               FillCommon + ProjectMeta
                                                       ▼
                                               metadata 自動帶上、sender 端自我驗證
```

**投影邏輯(抽值 / 排序 / encode / digest / overflow / nested mask 路徑)整段落在
provider lane、由 codegen 產;sender lane 完全不碰。** Hardcode 世界裡這段是 sender
自己手寫的 —— 而且每個 sender、每種語言、每個 transaction 各寫一次。

### 同樣 3 系統 / ~21 transaction,兩種做法

| | Hardcode(沒有本套件) | Autogen(本套件) |
|---|---|---|
| Provider 每系統 | 定義 proto(無標籤) | 定義 proto + **~5 行標籤** |
| 投影 / encode / digest / overflow | **每個 sender 手寫** | **codegen 產(人寫 0 行)** |
| Sender 每次呼叫 | 每 transaction 手寫抽取 + 設 header | **2 行** |
| 跨 N 個 sender / 多語言 | 各自重寫、各自維護、易漂移 | 共用一份 lib,0 重複 |
| body 改了 | 每處手動同步,漏一處就 silent drift | 重 codegen,全員自動跟上 |
| 規格 / 文件 | **沒有明確定義、沒文件**,各 sender 各自猜格式 → 不一致 | proto 標籤 + 共用合約**就是唯一定義 / 文件**,codegen 強制對齊 |
| 時程 / rollout | 每個 sender 要手改,**一改就拖很久、上線慢** | 改 proto 重 codegen,全員自動跟上,時程快 |
| 巢狀值(如 mask) | **每個 tx 各手寫一條巢狀路徑(像 xpath)去 parse body**(10+ 種路徑各一條)→ 極易出錯 | `walkProj` **自動產生正確 getter 路徑**;proto 改了路徑自動跟 |

### 代碼量(誰寫、寫幾次 —— autogen 只算 src,codegen 產的不算人寫)

| 項目 | Hardcode(人寫) | Autogen(人寫 = 只算 src) |
|---|---|---|
| 投影邏輯(by-tx 抽取 + 路徑) | 每個 sender 手寫,21 tx 各一塊 | **codegen 產,0 人寫**(範例輸出 335 行) |
| encode / sha256 / digest / overflow 機制 | ~150–230 行(沒共用就每 sender / 每語言重來) | 共用 helper,寫一次(算進下面 src) |
| 一次性平台 src(plugin + helper + 合約) | — | **616 行**,一次性 |
| 每系統 | proto(無標籤) | proto + **~5 行**標籤 |
| 每 sender | **~250–400 行**(機制 + 21 tx 投影)× 語言,要維護 | **2 行**(`FillCommon` 27 行共用) |

老實講幾個數字:
- **Hardcode 一個 sender 從零做完 21 tx ≈ 250–400 行**(機制 ~150–230 + by-tx 投影 ~100–170);就算把機制抽成共用 lib,**光「每個 tx 的抽取 + 路徑」也 ~100–170 行**,而且每 sender / 每語言重來、還要一直維護。
- **Autogen 人寫的只有 src**:一次性平台 616 行(plugin + helper + 合約)+ 每系統 ~5 行標籤 + 每 sender 2 行。**投影那 335 行是 codegen 產、不算人寫**(它正是 hardcode 要手寫的東西)。
- 所以重點不是「autogen 行數少」(它一次性 src 反而 616 行);而是 **autogen 投影 0 人寫、邊際 ≈ 0**;hardcode 是**每 sender × 每語言 ~數百行、且要維護**。sender / 改動越多,差距越大。

而且 hardcode 還有三個藏起來的成本:**規格沒定義也沒文件、每次改動都要 sender 重寫拖時程、
巢狀值要手寫路徑 parse body 容易錯** —— autogen 把這三個全變成編譯期保證(合約即文件、
改 proto 自動 rollout、`walkProj` 產路徑)。

---

## 9. 時程與工時(man-day,粗估)

> Rough sizing,假設 **3 系統、~2 個 sender、21 transaction**。重點是「成本**落在哪、會不會攤平**」,不是絕對值。

| 階段 | Autogen(本套件) | Hardcode | 說明 |
|---|---:|---:|---|
| ① API plugin dev | **~10**(一次性) | ~3 | autogen 做 plugin + helper + 合約;hardcode 至少要補「契約定義 + 共用 encode/digest lib」,否則就是「沒文件各自猜」 |
| ② 3 sys 改 proto | ~1.5 | ~1 | autogen 多貼標籤(~5 行/系統) |
| ③ APISIX 改 | ~0.5 | ~0.5 | 兩邊基本沒改(走 path / header 路由) |
| ④ sender 改 code | ~2 | **~18 ⚠ 大頭** | autogen:include + 2 行(**與 tx 數無關**);hardcode:每個 sender 手寫 **21 種**投影 + 巢狀路徑 + encode/digest —— **最花時間的一塊** |
| ⑤ 整合測試 | ~2.5 | **~9** | hardcode 手寫易錯,debug digest / 編碼 / 路徑 |
| **首次 rollout 合計** | **~16.5** | **~31.5** | |
| **邊際成本(每多 1 系統 / sender / 改動)** | **~0.5–1** | **重來 sender + test** | autogen 攤平,hardcode 不攤平 |

```
首次 rollout 工時(man-day)
Autogen   ██████████ plugin(一次性)  ▍proto ▏apisix ▍sender ▎test            ≈ 16.5
Hardcode  ▍lib ▏proto ▏apisix ██████████████████ sender    █████████ test    ≈ 31.5
                                  ▲ ④ sender 改 code 是大頭,每 sender/語言/改動都重來
```

要點:
- **Autogen 把成本前置到一次性 plugin(~10 md),之後邊際成本 ≈ 0**(貼標籤 / 2 行 / 重 codegen)。
- **Hardcode 省了 plugin,但 ④ sender 改 code 變成大頭(~18 md)**—— 21 種 transaction 每個 sender 各手寫一次,再加更重的測試;而且**每新增 sender / 語言 / 改動都重來**,不攤平。
- 這個規模(3 系統、21 tx、多 sender)**autogen 連首次 rollout 就省一半以上**(16.5 vs 31.5),而且到第 2、第 3 個 sender 或第 4 個系統,差距持續拉大(autogen 幾乎免費,hardcode 線性累加)。

---

## 10. 總比較表(多面向 + 優 / 平 / 輸)

> 用我們的**實際情境**評:**只有 1 個 sender、看首次 rollout、先不考慮後續改動** ——
> 所以 autogen「跨 sender 攤平 / 改動自動 rollout」這類長處在這情境先不算分。

分三個大面向,由重到輕:**① 品質 / 易出錯**(最重要)→ **② 時程 / Efforts** → **③ 延展性**(本情境較不重要)。

| 面向 | Hardcode(手寫) | Autogen(本套件) | 對 autogen |
|---|---|---|:--:|
| **① 品質 / 易出錯(最重要)** | | | |
| &nbsp;&nbsp;規格 / 文件 | 沒定義、沒文件,各自猜 | 標籤 + 合約 = 唯一、可執行的定義 | 優 |
| &nbsp;&nbsp;一致性 / 漂移 | 各自實作、無強制 → 易漂移 | 產 body 時一併投影到 header → **強制一致** | 優 |
| &nbsp;&nbsp;巢狀值抽取(mask) | 每個 tx 各自手寫巢狀路徑硬抽(像 XPath),欄位一改就漏 | `walkProj` 依 proto 自動生 getter 路徑 | 優 |
| &nbsp;&nbsp;錯誤處理(APISIX silent failure) | metadata 爆量 / 漏欄位 → APISIX 直接 reset / truncate,silent、難查 | build / sender / receiver 三關 + 7 KB overflow **顯式旗標**,絕不 silent | 優 |
| &nbsp;&nbsp;測試 | 手寫易錯、debug 重 | invariants 可測 + codegen 負面測試 | 優 |
| &nbsp;&nbsp;Sender 每次呼叫 | 每 tx 手寫投影 | include + 2 行 | 優(輕微)³ |
| **② 時程 / Efforts** | | | |
| &nbsp;&nbsp;工時 + 時程(21 tx 首次) | ~31.5 md,④ sender 改 code 拖最久 | ~16.5 md,改 proto 自動 rollout | 優 |
| &nbsp;&nbsp;Provider 每系統 | 定義 proto | proto + ~5 行標籤 | 平(多 5 行) |
| &nbsp;&nbsp;代碼量(人寫) | 一個 sender ~250–400 行 | 投影 0 人寫;一次性 src **616 行** | 輸 ¹ |
| **③ 延展性(本情境重要性較低)** | | | |
| &nbsp;&nbsp;改動 / 維護 + 新增系統 | 每處手動同步、重來 sender + test | 改 proto 重 codegen(+1 proto) | 平 ² |

¹ 1 sender 首次、plugin 從零做:autogen 一次性 src(616)反而比單一 hardcode sender(~350)**多**。但那是 reusable、投影 0 人寫、0 維護;**plugin 若已存在則此格轉大優**。
² 新增 / 改動時**兩邊都仍要動一處**:hardcode 改 sender + test,autogen 改 proto 重 codegen —— autogen 只改一處且自動 rollout,但本情境(1 sender、不考慮後續)先算平;**有第 2 sender 或開始改動即轉優**。
³ 與「巢狀值 / 代碼量」同源,每次呼叫的絕對差距小,不另列為大勝。
（原「跨語言 / 多 sender」面向因本情境只有 1 個 sender、用不到,已移除。）

> 結論:嚴格只看「**1 sender、首次、不計後續**」,autogen 是 **7 優 / 2 平 / 1 輸**
> —— 唯一輸的是「代碼量」(一次性 src 較多)。把後續改動 / 多 sender 算進來,2 個平也轉優。

---

**一句總結:Provider 貼一個標籤、走原本的 codegen;Sender include 一支 header、寫兩行;
header 保證是 body 的投影,錯了在來源就炸。**

> 設計細節與可測試的 invariants 見 [`CONTEXT.md`](CONTEXT.md);可跑的程式在
> [`example/`](example/);原始 spec / domain model 在 [`archive/`](archive/)。
