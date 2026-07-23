# sys4 範例 — UpdateMaterialRequest：多來源 lot id 正規化成 `LotID`

目標讀者：要接入「一個 method、多種互斥批次操作」形態系統的 proto owner 與 producer
（建構 request 的程式）owner。本文是設計定案 + 接入範例；已實作的對照系統見
`example/proto/sys1.proto`（同為 `[+meta]` 模式）。

---

## 1. 場景

`UpdateMaterialRequest` 一個 method 服務三種批次操作，各自用不同 message、不同欄位名
攜帶 lot id：

```proto
message IdChange {
  string orig_mat_id     = 1;
  string new_mat_id      = 2;
  string new_mat_lot_id  = 3;
}
message LotAdd {
  repeated string mat_id_list = 1;
  string new_mat_lot_id       = 2;
}
message LotChange {
  string orig_mat_lot_id = 1;
  string new_mat_lot_id  = 2;
}

message UpdateMaterialRequest {
  msgHeader msgHdr   = 1;
  string ctrl_job    = 2;
  string ope_no      = 3 [(routing.project)={key:"x-ope-no"}];
  string eqp_id      = 4 [(routing.project)={key:"x-eqp-id"}];
  string stage_id    = 5;
  string route_id    = 6;
  string tech        = 7;
  repeated IdChange  id_change  = 8;
  repeated LotAdd    lot_add    = 9;
  repeated LotChange lot_change = 10;

  // [+meta] 唯一新增欄位：唯一的 pctx 投影來源（§3）
  repeated common.v1.ProcessContext contexts = 11;
}
```

**核心前提（互斥假設）**：一個 request 語意上只會有 **一種** list 非空
（`id_change` / `lot_add` / `lot_change` 三選一），單一 list 內可多筆。
proto3 的 `oneof` 不能裝 repeated 欄位，所以這條不變量 schema 表達不出來，
由 producer 在 §3 強制執行（fail loud）。

## 2. 為什麼不能直接在三個 message 裡各標一個 pctx key

曾有草稿在三個 message 內各標 `(routing.pctx)`（key 分別為 `IdChangeLotId` /
`LotAddLotId` / `LotChangeLotId`）。這條路**編譯期就會失敗**，不是風格問題：

- SPEC §9：一個 message 只准**一個** repeated process-context 欄位。三個帶 pctx 的
  repeated 欄位直接被 `protoc --meta_out` 拒絕（negative test：
  `bad_multiple_ctx_fields.proto`）。
- 就算過得了，三個不同 key 也違背共用 7 欄位 schema——下游必須知道「這個系統的
  lot id 叫哪個 key」，正是本 kit 要消滅的 per-system 對照表。

正解：三個 message **不標任何 annotation**，lot id 統一經 `contexts` 投影成
canonical key `LotID`。

## 3. Producer 端：`FillContexts`

填 body 是 producer 的責任（ADR 0001 / CONTEXT.md 原則 #10）；kit 與 `Send<>()`
零改動。producer 在呼叫 `Send` **之前**填 `contexts`：

```cpp
// 回傳 false = 互斥假設被打破，producer 必須拒送（never silent）。
bool FillContexts(UpdateMaterialRequest& req) {
  int sources = !req.id_change().empty() + !req.lot_add().empty()
              + !req.lot_change().empty();
  if (sources > 1) return false;   // 絕不默默挑一個來源投影

  auto add = [&](const std::string& lot) {
    // 只填 LotID；其餘 6 欄留空，投影為 Key=（faithful，present-but-empty）
    req.add_contexts()->set_lot_id(lot);
  };
  for (const auto& e : req.id_change())  add(e.new_mat_lot_id());
  for (const auto& e : req.lot_add())    add(e.new_mat_lot_id());
  for (const auto& e : req.lot_change()) add(e.new_mat_lot_id());
  return true;
}
```

規則：

| 規則 | 內容 |
|---|---|
| canonical 值 | 三種 message 一律取 `new_mat_lot_id`（新 lot id） |
| 單位 | 一筆 item = 一行 context，**不去重**（同 lot 出現兩筆就投兩行，faithful projection） |
| 順序 | 依 list 內 body 序（互斥成立時只有一個來源，順序天然確定，digest 可重算） |
| 互斥破裂 | `sources > 1` → 拒送，比照 kit 失敗模型（blocking，never silent） |

## 4. Wire 上長什麼樣 / routing 怎麼用

`lot_add` 帶 2 筆、`ope_no="OP123"`、`eqp_id="EQP-A"` 時，sender
（`FillCommon` + `ProjectMeta`）emit：

```
x-ope-no: OP123                              ← (routing.project) scalar，整包一個值
x-eqp-id: EQP-A
x-process-context-count: 2
x-process-context-format: urlencoded-query-string-v1
x-process-context: ChamberId=&LotID=LOT001&OperationNO=&PartID=&RecipeID=&StageID=&Tech=
x-process-context: ChamberId=&LotID=LOT002&OperationNO=&PartID=&RecipeID=&StageID=&Tech=
x-process-context-digest: sha256:<64hex>
```

- **整包層級的分流**（依機台/站點）：APISIX 讀 `x-eqp-id` / `x-ope-no`——
  一個 request 一個值，適合當 route 條件。
- **lot 層級**（查詢、追蹤、對帳）：讀 repeated `x-process-context` 行內的
  `LotID=`。注意一個 request 有 N 行，**不適合**當單值 route 條件。
- 操作類型（add / change / idchange）**不在 metadata**：下游路由不依它分流
  （定案），需要的人讀 body。7 欄位 schema 凍結，不加 `Op`、不加 custom 欄位。

## 5. 上限與失敗行為（接入前必讀）

| 情況 | 行為 | 你要做什麼 |
|---|---|---|
| 一種 list、≤25 筆（**常態**，設計前提） | 正常投影，如 §4 | — |
| 互斥破裂（兩種 list 同時非空） | `FillContexts` 回 false，producer 拒送 | 修 client bug；這是 client 端錯誤 |
| >25 筆 / 單行 >512 B / 總量 >7168 B | `x-process-context-overflow: true`，context 行 + digest 全部抑制，count 照發（SPEC §5.4，policy 集中不為 sys4 開特例） | 下游看到 flag 就 fallback 讀 body。sys4 批量已確認 ≤25，此為防禦線非常態 |
| digest 不符 | receiver 拒收（header/body drift） | 照 `receiver_verify` 的既有流程 |

## 6. 對照實作（example/ 內可跑）

| 本文段落 | 實作 |
|---|---|
| §1 proto | `example/proto/sys4.proto` |
| §3 `FillContexts` | `example/sender/sys4_fill_contexts.h`（producer 端，非 kit） |
| §4 wire demo | `example/sender/unified_sender.cc` 的 sys4 區塊（`./build/unified_sender`） |
| §5 三個驗證 case | `example/tests/test_projection.cc` 的 sys4 區塊：單來源展開（含 exact 行 pin）、互斥破裂拒送、26 筆 overflow |

`cd example && ./build.sh` 一次跑完 codegen + 全部 assert。
