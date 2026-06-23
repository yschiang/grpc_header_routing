# gRPC Header Generation Kit — 方案建議(給 Sender 工程師)

> 版本: v0.2 · 提供方: Receiver Team · 對象: C++ Sender 工程師

## 1. 背景與問題

Sender 透過 `.proto` 產生 XML body 送到我們(receiver)。為了讓 **APISIX 能依內容做 routing**,每個 request 需額外帶三個 gRPC metadata header:

| Header (metadata key) | Cardinality | 說明 |
|---|---|---|
| `tool-header` | **1**(required) | 機台/腔體層級的 routing key |
| `lot-header` | **1..N** | 每個 Lot 一筆 |
| `mask-header` | **0..N** | 每個 Mask 一筆(已確認可能多筆) |

每筆 header 的 value 是 **URL-encoded、`&` 分隔的 key=value 串**,例如:

```
tool-header: EqpID=T01&ChamberId=C1
lot-header:  LotId=L001&Step=ST10&Recipe=R%2FA
lot-header:  LotId=L002&Step=ST20&Recipe=R%2FB
mask-header: MaskId=M99&LayerId=METAL1
```

這些值在 body 裡也都有。核心風險是:**body 與 header 兩份來源若不一致,APISIX 會 route 到錯的地方**,且這種錯誤是靜默的。

> **⚠️ Metadata 總大小上限(必讀)**
> gRPC 與 APISIX 對單一 request 的 metadata 總量有上限,**本文件保守假設為 8 KB**(gRPC `GRPC_ARG_MAX_METADATA_SIZE` 預設值,實際數字以 receiver/APISIX 端設定為準,見 OPEN-1)。
> `lot-header` 是 `1..N`、`mask-header` 是 `0..N`,**多筆累加極易撐爆此上限**。若一個 request 帶數十個 Lot,header 可能被截斷或整個 RPC 被拒,且失敗點在傳輸層,難 debug。
> Sender 應在送出前估算 header 總長;若 Lot/Mask 數量可能很大,需與 receiver 討論改用「只帶共通維度」而非逐筆列出(屬 routing 設計,見 §8)。

## 2. 設計原則:Body 為 SSOT,Header 從同一棵 message tree 衍生

我們不另外維護一份「欄位 → header」的映射檔(那等於開第二個 source,會漂移)。改為把映射意圖**標註在 proto 欄位上**(custom option),body 與 header 都從同一個 message 實例產出,結構上不可能不一致。Sender 改欄位時,映射就在欄位旁邊,不會漏改。

```
                ┌─→ XML serializer → body      (※ 由 sender 自理,不在本 kit 範圍)
Request 實例 ───┤
                └─→ header builder → tool / lot / mask-header (URL-encoded)  ← 本 kit 交付
```

> **範圍界定**:本 kit **只交付 header 生成**(option 定義 + 兩支 sample)。XML body 的序列化由 sender 既有流程負責;本 kit 不提供、也不規範 body 格式。SSOT 的保證來自「header 與 body 讀同一個 message 實例」,但 body 怎麼序列化是 sender 自己的事。

## 3. 交付內容

| 檔案 | 用途 | 誰維護 |
|---|---|---|
| `proto/header_options.proto` | custom option 定義(SSOT 契約) | **Receiver** |
| `proto/request.proto` | 標註範本,sender 照抄改自己的 message | 範本 |
| `sample-reflection/header_gen_reflection.cc` | **入門 sample**,runtime reflection | 範例 |
| `sample-plugin/header_gen_plugin.cc` | **production 建議**,protoc plugin codegen | 範例 |

## 4. 怎麼標註欄位

import 我們的 option 定義後,在 scalar 欄位上標 `group` 和 `key`:

```protobuf
import "header_options.proto";

message Request {
  string eqp_id     = 1 [(routing.header) = {group: TOOL, key: "EqpID",     order: 1}];
  string chamber_id = 2 [(routing.header) = {group: TOOL, key: "ChamberId", order: 2}];
  repeated Lot  lots  = 3;   // -> lot-header (1..N)
  repeated Mask masks = 4;   // -> mask-header (0..N)
  string operator_id = 5;    // 沒標 → 只進 body,不進 header
}
```

規則:

1. **metadata key 不用自己填** —— 由 `group` 固定成 `tool-header` / `lot-header` / `mask-header`(全小寫,gRPC 要求)。
2. **cardinality 由 proto 結構表達** —— TOOL 放頂層出 1 筆;LOT/MASK 用 `repeated` 自然就是多筆。
3. **`key` 只標 routing 需要的欄位** —— body-only 欄位不標。
4. **`order`(選填,best-effort)** —— 控制同一 group 內 key 的輸出順序,**僅為可讀性/穩定性,不影響 routing 正確性**(receiver 端以 key 查找解析,不依賴順序)。框架會盡量按 order 由小到大輸出;未填則用 proto field number 排序。
   - ⚠️ proto3 限制:scalar 預設值為 0,框架把 **`order: 0` 視同「未填」**。因此請從 `1` 起編號,**不要使用 `order: 0`**。
5. **目前僅支援 scalar 欄位**(string / int / bool 等)進 header,不支援巢狀 message。

## 5. 兩種 sample 怎麼選

**Sample A — Reflection(入門)**
- 不需改 build、不需 plugin,link message proto 即可跑。
- 適合先把鏈路跑通、低頻發送、PoC。
- 代價:每次發送遍歷 descriptor,效能較差;泛型遍歷任意 message tree 較麻煩。

**Sample B — protoc plugin codegen(production 建議)**
- build time 把映射固化成直接的 `AddMetadata` 呼叫,zero reflection、type-safe、最快。
- 接進 CMake/Bazel 後,「改 `.proto` → 重新 codegen → header builder 自動跟上」成為 build 的一環,這正是這套機制真正擋住「漏改 header」的地方。
- 代價:要把 plugin binary 掛進 build。

> 建議路徑:先用 A 驗證行為與 receiver/APISIX 對齊,online-critical 路徑切到 B。

## 6. 發送端套用(兩種 sample 的對外介面一致)

```cpp
demo::Request req;
req.set_eqp_id("T01");
req.set_chamber_id("C1");
auto* lot = req.add_lots();
lot->set_lot_id("L001"); lot->set_step("ST10"); lot->set_recipe("R/A");
auto* mask = req.add_masks();
mask->set_mask_id("M99"); mask->set_layer_id("METAL1");

grpc::ClientContext ctx;
headergen::ApplyHeaders(req, &ctx);   // 同一份 req 衍生所有 header
stub->YourRpc(&ctx, req, &resp);
```

`lot-header` 對同一 key 呼叫多次 `AddMetadata`,wire 上即 repeated metadata,正合 1..N / 0..N。

## 7. 格式約定(sender / receiver 必須一致)

- **為什麼一定要 encode(不是只為了分隔)**:非 `-bin` 結尾的 gRPC metadata value **只允許可印 ASCII(0x20–0x7E)**。中文 recipe 名、特殊符號若直接塞會違規或被丟棄。URL-encode 後所有 byte 落在合法 ASCII 範圍,因此 encode 是**硬性要求**,不是可選的美化 —— 即使 value 看起來「乾淨」也必須 encode。
- **URL-encode 規則**:RFC 3986 unreserved(`A-Z a-z 0-9 - _ . ~`)不編碼,其餘 `%XX`。
- **空白固定編成 `%20`**,不使用 `+`。
- **key 與 value 都 encode**,防止欄位值含 `&` / `=` / `%` 破壞分隔。
- **metadata key 一律小寫**。

這些已集中實作在 sample 的 `UrlEncode()`,sender 不需重寫,但要確認 receiver decode 行為與此一致。

## 8. APISIX 多值 header 的提醒(routing 設計要先確認)

`lot-header` / `mask-header` 有多筆時,APISIX 收到同名 header **預設可能逗號合併**(HTTP 標準行為):

```
lot-header: LotId=L001&Step=ST10, LotId=L002&Step=ST20
```

所以 routing rule 到底是 match 單筆 Lot 的某欄位,還是要遍歷所有 Lot,會決定該不該讓它合併、以及 parse 方式。

> **給 sender 的目前假設(在 OPEN-1 拍板前照此行事)**:假設 routing 以**逐筆 repeated metadata**形式消費(每個 Lot/Mask 各一筆,不靠逗號合併解析)。sample code 即按此產出。若 receiver/APISIX 端最終採不同策略,會更新本文件並通知;sender 端**不需預先處理合併邏輯**。

## 9. 驗證

本 kit 的 proto + option 已驗證可編譯,且 option 值能正確寫入 descriptor 並讀回(group/key/order 一致)。sender 端可用以下指令自行驗證標註是否生效,不需先寫 plugin:

```bash
protoc --descriptor_set_out=out.desc --include_imports request.proto
# 解 out.desc 檢查 field options 內的 (routing.header) extension
```

## 10. 版本與相容性

本 kit 是要長期發給 sender 使用的契約,演進規則:

- **Extension number `50001` 為穩定保留值**,登記在 receiver 的 proto registry,不會變更(見 OPEN-4)。
- **加欄位相容**:sender 在自己的 message 新增標註欄位,屬向後相容,不影響既有 routing。
- **改 group 語意 = breaking change**:receiver 若調整 `tool/lot/mask-header` 的含義或拆併 group,會 bump `header_options.proto` 的版本並提前通知;sender 不應假設 group 語意永久不變。
- **格式約定凍結**:§7 的 URL-encode 規則一旦上線即凍結,變更需雙方協調(否則 decode 會錯)。
- 本文件版本與 `header_options.proto` 的相容性以文件頂端版本號追蹤。

## 11. 待確認事項(OPEN)

- **OPEN-1** APISIX 對 repeated `lot-header` / `mask-header` 的實際處理(逐筆 vs 逗號合併 vs 取首筆)— 需實測;同時確認 metadata 總大小上限的實際數字(§1 暫用 8 KB 假設)。
- **OPEN-2** Lot/Mask 數量上界,以及超過 header 大小上限時的退化策略(只帶共通維度?)。
- **OPEN-3** scalar 之外是否未來會需要巢狀 message 進 header(目前不支援)。
- **OPEN-4** extension number `50001` 正式登記到 proto registry,確保不與其他 option 衝突。
