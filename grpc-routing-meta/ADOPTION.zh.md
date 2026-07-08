# ADOPTION — 把 kit 帶回 local env，接上真實 sys2 RMS proto

目標讀者：RMS owner（receiver）與 sender owner。照本文從零走到「真實 proto build 過、
headers 上線、receiver 驗過」。demo 版對照：`example/proto/sys2.proto` +
`example/sender/unified_sender.cc` 的兩個 sys2 pattern + `example/tests/test_projection.cc`。

---

## 0. 要帶走什麼

整個 `example/` 目錄是 self-contained，直接複製走即可。kit 本體只有四塊：

| 帶走 | 是什麼 | 誰要讀 |
|---|---|---|
| `proto/metadata_options.proto` | `(routing.project)` / `(routing.pctx)` 兩個 tag 的定義 | 改 proto 的人 |
| `proto/process_context.proto` | 共用 ProcessContext（可選，見 §2 路線 B） | 改 proto 的人 |
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

## 2. 接上真實 RMS proto（worked example）

以真實的 NRMS 交易為例：

```proto
message Typ_NRMSLotInfo {
    string LotId = 1;
    ...
}
message rqst_NRMS_GetRecipeSet {
    msgHeader msgHdr = 1;
    string eqpId = 2;
    string recipeId = 3;
    int32 levelNo = 4;
    ...
    repeated Typ_NRMSLotInfo LotInfo = 7;
    ...
}
```

### 2.1 每個值先分類（tag 決策表)

| 值 | 分類 | 做法 |
|---|---|---|
| eqpId（tool id） | sender 在 call 時就知道 | **不 tag**。走 `Runtime.tool_id` → `x-tool-id`（Layer 1），body 欄位原封不動 |
| recipeId，單值 | body 裡、要給 gateway 路由 | `(routing.project) = {key:"x-recipe-id", required:true}` |
| LotInfo 裡的 LotId，N 值 | 多值 → 不能進單值 header | `(routing.pctx)`，見下 |
| N 個元素保證同一值的欄位 | batch 不變量 | `(routing.project)` + `uniform_across_repeated:true`（元素 0 投影、其餘硬驗，分歧 = blocking error） |
| 其他（levelNo、msgHdr…） | 與路由無關 | 不動，codegen 自動忽略 |

**判斷口訣**：sender 自己就知道 → Runtime；body 裡的單值 → project；body 裡的多值 → pctx。

### 2.2 多 lot 兩條路線

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

> 這個例子是 repo 裡**可 build 的 fixture**：`example/proto/nrms.proto`（已在
> `build.sh` 的 SYSTEMS 與 CMake 清單裡），對應斷言在 `example/tests/test_projection.cc`
> 的 nrms 區塊。照抄它就是路線 A 的完整範本。camelCase 欄位名可直接用——生成碼
> 會用 protobuf 的小寫 getter（`recipeId` → `recipeid()`）。

**路線 B — 加共用 `repeated common.v1.ProcessContext contexts`（平台統一 schema）**

各系統 context 的 key 集合完全一致（LotID/RecipeID/Tech/…，7 keys），gateway 只需認一種
格式。代價是 request 加一個新欄位、lot 資料要多填一份。demo 的 sys2 就是這條
（`example/proto/sys2.proto`）。**建議**：先用 A 驗證可套用，平台要求統一 context schema 時再換 B。

### 2.3 Build

```bash
cp nrms.proto example/proto/
# build.sh 裡一行：SYSTEMS=(sys1 sys2 sys3) → SYSTEMS=(sys1 sys2 sys3 nrms)
# CMake 路徑同理：CMakeLists.txt 的 foreach(name sys1 sys2 sys3) 加上 nrms
./build.sh
```

產出 `build/generated/nrms.proj.h` / `.cc`：每個有 tag 的 message 一個
`routingmeta::ProjectMeta(const rqst_NRMS_GetRecipeSet&, MetadataSink&, bool emit_digest = true)`。

tag 放錯 codegen 會**直接 fail 並說原因**（never silent）。常見訊息對照：

| codegen 錯誤訊息（節錄） | 意思 | 修法 |
|---|---|---|
| `is set under repeated field` | project 掛在 repeated 子樹下 | 多值改 pctx；真是 batch 同值加 `uniform_across_repeated` |
| `duplicate (routing.project) key` | 同 message 兩個欄位投同一 header | 留一個 |
| `must be a string scalar` | tag 掛在 int/message 上 | project/pctx 只吃 string scalar |
| `more than one repeated process-context` | 兩個 repeated 欄位都有 pctx | 只能一個（第二個會被靜默丟掉，所以擋掉） |
| `has no repeated ancestor` | uniform 掛在非 repeated 路徑 | 拿掉 flag 或改 project |

## 3. Sender owner 要寫的（共 2 行）

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
#include "nrms.proj.h"               // 生成的 ProjectMeta

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

## 4. Receiver（RMS）owner 要看的

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

## 5. 「能不能直接套用」驗收清單

1. `./build.sh` 全綠（negative codegen gate + test_projection + receiver_verify）。
2. 真實 proto 加進 SYSTEMS 後 build 過 —— tag 分類就是對的。
3. 拿一筆真實交易資料填 request，用 `VectorSink` dump（照抄 `unified_sender.cc` 的
   `dump()`）—— 逐行核對 §4 的表。
4. 空 recipeId 的 request 跑一次 —— 應得 `ok=false` + `x-routing-error`，process 不死。
5. 一個 FOUP 塞滿 25 lot —— count=25、25 行 context、digest 在；塞 26 個 → overflow 行為。
6. （有 grpc++ 的環境）`grpc_demo/run.sh` 跑真線：clean call 過、tampered call 被拒。

全過 = 可以直接套用；卡在哪一步，對照 §2.3 的錯誤表或 `DEMO.md` 的完整 session 輸出。
