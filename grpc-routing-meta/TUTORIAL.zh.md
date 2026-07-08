# TUTORIAL — 把 kit 帶回 local env，接上真實 sys2 RMS proto

目標讀者：RMS owner（receiver）與 sender owner。照本文從零走到「真實 proto build 過、
headers 上線、receiver 驗過」。demo 版對照：`example/proto/sys2.proto` +
`example/sender/unified_sender.cc` 的兩個 sys2 pattern + `example/tests/test_projection.cc`。

---

## 0. 要帶走什麼

整個 `example/` 目錄是 self-contained，直接複製走即可。kit 本體只有四塊：

| 帶走 | 是什麼 | 誰要讀 |
|---|---|---|
| `proto/metadata_options.proto` | `(routing.project)` / `(routing.pctx)` 兩個 tag 的定義 | 改 proto 的人 |
| `proto/process_context.proto` | 共用 ProcessContext（可選，見 §3 路線 B） | 改 proto 的人 |
| `src/plugin/protoc-gen-meta.cc` | codegen plugin：tag → `ProjectMeta()` | 不用讀，build 會編 |
| `src/common/*.h` | runtime kit：sink、common headers、digest、parser | sender / receiver |

其餘（`sender/`、`receiver/`、`tests/`、`grpc_demo/`）是可執行的參考範例，建議一起帶走當教材。

## 1. Local env 需求

- C++17 編譯器（clang++ 或 g++）。
- protoc + libprotobuf + libprotoc，**同一版本**（3.20 / 3.21 皆測過；CI matrix 就是這兩版）。
- 除 protobuf 外零依賴。真線 demo（`grpc_demo/run.sh`）才需要 grpc++ + grpc_cpp_plugin。

```bash
# 驗環境
protoc --version                 # libprotoc 3.20.x / 3.21.x
# 工具不在 PATH 時可覆寫：
PROTOC=/opt/protobuf/bin/protoc CXX=clang++ ./build.sh
```

`build.sh` 用 pkg-config（沒有就從 protoc 位置推 prefix），不吃硬編路徑。

## 2. 先跑既有的 ms，看懂一次完整流程

repo 內建一個 **ms（management system）fixture**：`example/proto/ms.proto`。它就是
真實 fab 交易的形狀——camelCase 欄位、自家的 repeated lot message——而且已經 tag 好、
接進 build 和測試。先跑它、看懂 in/out，再加你自己的系統。

```proto
// example/proto/ms.proto（全文就這麼短；[+meta] 只有 1 個 import + 2 行 tag）
import "metadata_options.proto";                            // [+meta]

message Typ_MSLotInfo {
  string LotId = 1 [(routing.pctx) = {key:"LotID"}];        // [+meta] 多值 → context 行
  string carrierId = 2;
}
message rqst_MS_GetRecipeSet {
  msgHeader msgHdr = 1;
  string eqpId = 2;                                         // 不 tag：tool id 走 Runtime
  string recipeId = 3 [(routing.project) = {key:"x-recipe-id", required:true}];  // [+meta] 單值 → header
  int32 levelNo = 4;
  ...
  repeated Typ_MSLotInfo LotInfo = 7;
}
```

```bash
cd example && ./build.sh        # 全綠 gate：codegen 驗證 + 測試 + receiver
```

輸出裡跟 ms 有關的兩行：

```text
[gen ] ms.proto (cpp + meta)     ← plugin 驗證 tag、產生投影碼
[test] run test_projection      → ALL TESTS PASSED（含 ms 區塊）
```

然後看三個地方，流程就通了：

1. **tag 換到了什麼** — `build/generated/ms.proj.h`：一個
   `ProjectMeta(const ms::v1::rqst_MS_GetRecipeSet&, MetadataSink&, ...)`，投影碼全是生成的。
2. **填什麼、出什麼** — `tests/test_projection.cc` 的 ms 區塊；完整 sender in → wire →
   receiver out 在 §6 案例 4。
3. **上線時 sender 怎麼接** — `sender/unified_sender.cc`（§4，就 2 行）。

## 3. 一步步加你自己的 management system（例：rms）

現在拿你的真實 proto 照做一次。以下用 NRMS 的 `rqst_NRMS_GetRecipeSet` 當例子，
最後的成品長什麼樣，對照 `ms.proto` 就是了（它就是照這流程做出來的）。
每一步做錯 build 都會 fail loud 告訴你原因，不會靜默出錯的 header。

### 3.0 放進 build

```bash
cp rms.proto example/proto/
# build.sh 裡一行：SYSTEMS=(sys1 sys2 sys3 ms) → 加上 rms
# CMake 同理：CMakeLists.txt 的 foreach(name sys1 sys2 sys3 ms) 加上 rms
```

### 3.1 每個值先分類（tag 決策表)

| 值 | 分類 | 做法 |
|---|---|---|
| eqpId（tool id） | sender 在 call 時就知道 | **不 tag**。走 `Runtime.tool_id` → `x-tool-id`（Layer 1），body 欄位原封不動 |
| recipeId，單值 | body 裡、要給 gateway 路由 | `(routing.project) = {key:"x-recipe-id", required:true}` |
| LotInfo 裡的 LotId，N 值 | 多值 → 不能進單值 header | `(routing.pctx)`，見下 |
| N 個元素保證同一值的欄位 | batch 不變量 | `(routing.project)` + `uniform_across_repeated:true`（元素 0 投影、其餘硬驗，分歧 = blocking error） |
| 其他（levelNo、msgHdr…） | 與路由無關 | 不動，codegen 自動忽略 |

**判斷口訣**：sender 自己就知道 → Runtime；body 裡的單值 → project；body 裡的多值 → pctx。

### 3.2 多 lot 兩條路線

**路線 A — 原地 tag 你既有的 LotInfo（最小改動，先用這條測可行性）**

plugin 不綁死共用 ProcessContext：任何 repeated message 的 string 欄位都能掛
`(routing.pctx)`（一個 request 限一個 pctx-bearing repeated 欄位，多了 codegen 會擋）。

```proto
import "metadata_options.proto";                    // [+meta] 唯一新 import

message Typ_NRMSLotInfo {
    string LotId = 1 [(routing.pctx) = {key:"LotID"}];   // [+meta] 一行
    ...                                                   // 其他欄位不動
}
message rqst_NRMS_GetRecipeSet {
    msgHeader msgHdr = 1;
    string eqpId = 2;
    string recipeId = 3 [(routing.project) = {key:"x-recipe-id", required:true}];  // [+meta] 一行
    ...
    repeated Typ_NRMSLotInfo LotInfo = 7;           // 不動
    ...
}
```

改動總計：1 個 import + 2 行 tag。欄位、編號、結構全部原樣。

> 對照組就是 §2 跑過的 `ms.proto` ——它就是路線 A 的成品，照抄即可。camelCase
> 欄位名可直接用——生成碼會用 protobuf 的小寫 getter（`recipeId` → `recipeid()`）。

**路線 B — 加共用 `repeated common.v1.ProcessContext contexts`（平台統一 schema）**

各系統 context 的 key 集合完全一致（LotID/RecipeID/Tech/…，7 keys），gateway 只需認一種
格式。代價是 request 加一個新欄位、lot 資料要多填一份。demo 的 sys2 就是這條
（`example/proto/sys2.proto`）。**建議**：先用 A 驗證可套用，平台要求統一 context schema 時再換 B。

### 3.3 Build，讀懂 fail-loud

```bash
./build.sh
```

過了就會有 `build/generated/rms.proj.h` / `.cc`：每個有 tag 的 message 一個
`routingmeta::ProjectMeta(const rqst_NRMS_GetRecipeSet&, MetadataSink&, bool emit_digest = true)`。

tag 放錯 codegen 會**直接 fail 並說原因**（never silent）。常見訊息對照：

| codegen 錯誤訊息（節錄） | 意思 | 修法 |
|---|---|---|
| `is set under repeated field` | project 掛在 repeated 子樹下 | 多值改 pctx；真是 batch 同值加 `uniform_across_repeated` |
| `duplicate (routing.project) key` | 同 message 兩個欄位投同一 header | 留一個 |
| `must be a string scalar` | tag 掛在 int/message 上 | project/pctx 只吃 string scalar |
| `more than one repeated process-context` | 兩個 repeated 欄位都有 pctx | 只能一個（第二個會被靜默丟掉，所以擋掉） |
| `has no repeated ancestor` | uniform 掛在非 repeated 路徑 | 拿掉 flag 或改 project |

### 3.4 驗證你的新系統

最快的方法：抄 `tests/test_projection.cc` 的 ms 區塊，改成你的 message 名和期望值，
`./build.sh` 會自動跑。或照 §6 案例的做法用 `VectorSink` dump 出 headers 逐行核對。

## 4. Sender owner 要寫的（共 2 行）

你本來的 call site：

```cpp
rqst_NRMS_GetRecipeSet req;
// ... 填 eqpId / recipeId / LotInfo（本來就要填的業務資料）...
grpc::ClientContext ctx;
stub->GetRecipeSet(&ctx, req, &resp);
```

加上 kit：

```cpp
#include "common/common_headers.h"   // Runtime + FillCommon
#include "common/metadata_sink.h"    // GrpcSink
#include "rms.proj.h"                // 生成的 ProjectMeta（你在 §3 加的系統）

Runtime rt{corr_id, site_id, tool_id, unique_req_id, "rms"};  // 你本來就有的值
routingmeta::GrpcSink sink(&ctx);                             // [+meta] 1
routingmeta::ProjResult r = Send(req, rt, sink);              // [+meta] 2 (= FillCommon + ProjectMeta)
```

`Send` 的範本在 `example/sender/unified_sender.cc`（一個 template，全系統共用，
orchestration 歸 sender 所有，見 `docs/adr/0001`）。`ProjResult` 的處理原則：

- `r.ok == false` → 有 blocking issue（如 required 欄位空）。kit **不會 throw**、不會出空
  header，已在 metadata 放了 `x-routing-error: missing:x-recipe-id`。要不要中止 call 是
  sender 的 policy，不是 kit 的。
- `r.issues` 每筆有 kind（MissingRequired / Overflow / Inconsistent）+ key，接你的 log。
- Overflow（context 超過 25 個或總量超 7KB）非 blocking：count/overflow header 照出，
  context 行抑制，receiver 回頭讀 body。

## 5. Receiver（RMS）owner 要看的

wire 上會多這些 headers（gateway 路由用，backend 詳情永遠以 body 為準）：

| header | 來源 | 例 |
|---|---|---|
| `x-request-id` … `x-tool-id`（6 個 common） | Runtime | `x-tool-id: ETCH01` |
| `x-recipe-id` | body recipeId 投影（URL-encoded） | `RCP_ETCH_V3` |
| `x-process-context`（0..N 行，body 順序） | 每個 LotInfo 一行，key 排序、`Key=UrlEncode(V)` | `LotID=LOT01` |
| `x-process-context-count` | 恆出，= body repeated size | `3` |
| `x-process-context-format` | 恆出，常數 | `urlencoded-query-string-v1` |
| `x-process-context-digest` | count>0 且未 overflow 時 | `sha256:…` |
| `x-routing-error` | 僅投影失敗時 | `missing:x-recipe-id` |

receiver 端驗證用同一組 header：

```cpp
#include "common/process_context_parser.h"

auto vr = routingmeta::VerifyDigest(context_lines, digest_header);  // digest 缺席 → OK（verify-if-present）
auto kv = routingmeta::ParseContext(context_lines[0]);              // kv["LotID"] == "LOT01"（已 decode）
```

可執行參考：`example/receiver/receiver_verify.cc`（含竄改偵測），`./build.sh` 每次都會跑它。

## 6. 實跑測試案例 — sender in → wire → receiver out

以下輸出**不是示意，是實跑結果**：wire dump 來自 `./build/unified_sender`，
receiver 輸出來自 `./build/receiver_verify`，斷言值來自 `./build/test_projection`
（30+ 斷言，`./build.sh` 每次全跑，目前全綠）。common 6 headers（x-request-id 等）
每案都在，下面只列跟案例有關的行。

### 案例 1 — tool id + 單一 recipe id（`sys2.recipe.verify`）

```cpp
// sender in
Runtime rt{"CORR-LOT01-002", "F18", "ETCH01", "REQ-0002", "eap"};
sys2::v1::VerifyRequest req;
req.set_recipe_id("RCP_ETCH_V3");
```
```text
# wire out（實際 dump，ok=true）
x-tool-id:                 ETCH01
x-recipe-id:               RCP_ETCH_V3
x-process-context-count:   0
x-process-context-format:  urlencoded-query-string-v1
```
receiver 端：直接讀 `x-recipe-id` 路由；count=0 → 無 context、無 digest（結構 header 仍在，不是漏了）。

### 案例 2 — recipe id + 3 lots in one FOUP（`sys2.recipe.download`）

```cpp
// sender in
req.set_recipe_id("RCP_ETCH_V3");
for (const char* lot : {"LOT01", "LOT02", "LOT03"})
  req.add_contexts()->set_lot_id(lot);          // 只填 LotID（sparse）
```
```text
# wire out（實際 dump，ok=true）
x-recipe-id:               RCP_ETCH_V3
x-process-context-count:   3
x-process-context-digest:  sha256:dcddb76b4f04369106735d95a82fb44afe0bd8a6ebd0e9f248276b4c50e90799
x-process-context:         ChamberId=&LotID=LOT01&OperationNO=&PartID=&RecipeID=&StageID=&Tech=
x-process-context:         ChamberId=&LotID=LOT02&OperationNO=&PartID=&RecipeID=&StageID=&Tech=
x-process-context:         ChamberId=&LotID=LOT03&OperationNO=&PartID=&RecipeID=&StageID=&Tech=
```
```cpp
// receiver out（test_projection 斷言，全過）
VerifyDigest(context_lines, digest).ok == true       // header 沒被動過
ParseContext(context_lines[2])["LotID"] == "LOT03"   // 已 url-decode，body 順序
```
沒填的欄位出 `Key=`（present-but-empty）——這是 body 的忠實投影，receiver 不用猜「空」和「漏」。

### 案例 3 — recipe 沒填（required 失敗，不 throw）

```cpp
// sender in：recipe_id 忘了 set
sys2::v1::VerifyRequest req;
```
```text
# wire out（ok=false, issue=MissingRequired）
x-routing-error:           missing:x-recipe-id
（x-recipe-id 不出現 —— 絕不發空 header）
```
receiver / gateway 端：看到 `x-routing-error` 就知道投影失敗、原因是什麼；sender process 不會死，要不要送由 sender policy 決定。

### 案例 4 — ms fixture：真實形狀（路線 A：camelCase + 自家 LotInfo）

```cpp
// sender in（proto/ms.proto；注意 getter 是小寫：set_recipeid）
ms::v1::rqst_MS_GetRecipeSet req;
req.set_eqpid("ETCH01");                              // 沒 tag → 只留在 body
req.set_recipeid("RCP/V3");                           // 有 '/'，看 encoding
for (const char* l : {"LOT01", "LOT02", "LOT03"}) req.add_lotinfo()->set_lotid(l);
```
```text
# wire out（test_projection 斷言，全過）
x-recipe-id:               RCP%2FV3          ← '/' 被 url-encode
x-process-context-count:   3
x-process-context:         LotID=LOT01       ← 自家 message：只出有 tag 的 key
x-process-context:         LotID=LOT02
x-process-context:         LotID=LOT03
```
```cpp
// receiver out
VerifyDigest(cs, dg).ok == true
ParseContext(cs[2])["LotID"] == "LOT03"
```
跟案例 2 對照：路線 A 每行只有 `LotID=`（你 tag 了什麼出什麼），路線 B 是統一的 7-key 格式。

### 案例 5 — receiver 竄改偵測（`receiver_verify` 實際輸出）

```text
[accept] digest check: OK (header matches body)
  expected: sha256:efafba16…    actual: sha256:efafba16…

[reject] tampered body (CH-A->CH-X): rejected (mismatch caught)
  expected: sha256:efafba166aabd1be8ef91d0751220f106077b06d14940254322a23da966bd1dd
  actual:   sha256:21a25e0cd63d2342ee9c4773ff43c34a7a1feda1fad3101e90ffba1651497ba1
  error: digest mismatch: header/body projection drift

result: PASS (clean accepted, tampered rejected)
```
中途有人動了 context header（或 header/body 漂移），digest 對不上 → receiver 拒收，不會靜默吃下去。

## 7. 「能不能直接套用」驗收清單

1. `./build.sh` 全綠（negative codegen gate + test_projection + receiver_verify）。
2. 真實 proto 加進 SYSTEMS 後 build 過 —— tag 分類就是對的。
3. 拿一筆真實交易資料填 request，用 `VectorSink` dump（照抄 `unified_sender.cc` 的
   `dump()`）—— 逐行核對 §5 的表。
4. 空 recipeId 的 request 跑一次 —— 應得 `ok=false` + `x-routing-error`，process 不死。
5. 一個 FOUP 塞滿 25 lot —— count=25、25 行 context、digest 在；塞 26 個 → overflow 行為。
6. （有 grpc++ 的環境）`grpc_demo/run.sh` 跑真線：clean call 過、tampered call 被拒。

全過 = 可以直接套用；卡在哪一步，對照 §3.3 的錯誤表或 `DEMO.md` 的完整 session 輸出。
