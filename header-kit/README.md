# gRPC Header Generation Kit — Handoff

讓 C++ **sender** 在用 `.proto` 產 XML body 的同時,從**同一份 message 實例**衍生三個 gRPC metadata header(`tool-header` / `lot-header` / `mask-header`),供 **APISIX content-based routing**。本 kit 由 **receiver** 端維護並交付給 sender。

核心保證:header 與 body 讀同一棵 message tree,映射意圖綁在 proto 欄位上 → **結構上無法漂移,plugin 版更把漏改擋在編譯期**。

---

## 讀的順序

| # | 檔案 | 讀者 | 內容 |
|---|---|---|---|
| 1 | `SUMMARY.md` | 管理層 + 工程師 | 一頁解決方案報告(why / 容量驗證 / 結論) |
| 2 | `core-diagram.txt` | 所有人 | 一張圖看懂核心解法(30 秒) |
| 3 | `REQUIREMENTS.md` | 工程師 / architect | FR/NFR 逐條 spec + 追蹤矩陣 |
| 4 | `CONTEXT.md` | 接手者 / architect / AI agent | 需求脈絡、ADR、被否決方案、硬約束、詞彙 |
| 5 | `PROPOSAL.md` | sender 工程師 | 怎麼標 proto、兩種 sample 怎麼選、格式約定 |
| 6 | `proto/` | sender | `header_options.proto`(契約)+ `request.proto`(範本) |
| 7 | `sample-reflection/` | sender | 入門 sample(runtime reflection,不用改 build) |
| 8 | `sample-plugin/` | sender | production sample(protoc plugin codegen) |
| 9 | `common/` | sender | `url_encode.h` — 兩支 sample 共用的 encode 實作 |

---

## 檔案樹

```
header-kit/
├─ README.md                              ← 你在這
├─ SUMMARY.md                             一頁解決方案報告(管理層 + 工程師)
├─ REQUIREMENTS.md                        FR/NFR 逐條 spec + 追蹤矩陣
├─ core-diagram.txt                       核心解法 ASCII 圖
├─ CONTEXT.md                             需求脈絡 / 決策記錄(內含同張圖於 §8)
├─ PROPOSAL.md                            給 sender 的方案 + 用法
├─ proto/
│  ├─ header_options.proto                custom option 定義(SSOT 契約,receiver 維護)
│  └─ request.proto                       標註範本(sender 照抄改自己的 message)
├─ sample-reflection/
│  └─ header_gen_reflection.cc            SAMPLE A:runtime reflection(入門)
├─ sample-plugin/
│  └─ header_gen_plugin.cc                SAMPLE B:protoc plugin codegen(production)
└─ common/
   └─ url_encode.h                        共用 URL-encode(single source,header-only)
```

---

## 30 秒上手(sender)

1. 把 `header_options.proto` 加進你的 proto include path。
2. 在自己的 message 欄位上標註(照 `request.proto` 範本):
   ```protobuf
   import "header_options.proto";
   message Request {
     string eqp_id = 1 [(routing.header) = {group: TOOL, key: "EqpID"}];
     repeated Lot  lots  = 2;   // -> lot-header  1..N
     repeated Mask masks = 3;   // -> mask-header 0..N
   }
   ```
3. 選一支 sample 產 header:
   - **入門**:抄 `sample-reflection`,link message proto 即可跑。
   - **production**:用 `sample-plugin` 生成 type-safe builder,接進 CMake/Bazel。
4. 發送:
   ```cpp
   grpc::ClientContext ctx;
   headergen::ApplyHeaders(req, &ctx);   // 同一份 req 衍生所有 header
   stub->YourRpc(&ctx, req, &resp);
   ```

---

## 驗證狀態

| 項目 | 狀態 |
|---|---|
| `header_options.proto` + `request.proto` 編譯 | ✅ protobuf 3.21.12 |
| option 值寫入 descriptor 並讀回(group/key/order) | ✅ |
| 共用 `url_encode.h` 單元測試(含中文 byte-wise、`%20`、unreserved) | ✅ |
| plugin 編譯 + 執行 + 生成 code 編譯 + 輸出比對範例 | ✅ byte-identical |
| plugin 對非 scalar 欄位在 build 時報錯 | ✅ protoc 失敗並給明確訊息 |
| reflection sample 編譯(改用共用 encode 後) | ✅ |
| 多層 package → C++ namespace(`a.b.c` → `a::b::c`) | ✅ |

---

## ⚠️ 開放事項(必看,見 PROPOSAL §11 / CONTEXT §9)

- **OPEN-1** APISIX 對 repeated header 的實際消費方式(逐筆 / 逗號合併)+ metadata 大小上限**實際數字**(文件暫用 **8 KB 假設**)。
- **OPEN-2** Lot/Mask 數量上界與超限退化策略。
- **OPEN-3** 未來是否需要巢狀 message 進 header(目前僅 scalar)。
- **OPEN-4** extension number `50001` 正式登記到 proto registry。

> 目前對 sender 的行事假設:routing 以**逐筆 repeated metadata** 消費,sender 不需處理合併邏輯(待 OPEN-1 拍板)。

---

## 已知限制

- **僅 scalar 欄位**可進 header(string/int/bool 等);巢狀 message 不支援(會在 plugin build 時報錯)。
- reflection 版的 `ApplyHeaders` 為求清楚,對 `Request` 的 repeated 欄位名(`lots`/`masks`)寫死,非完全泛型;要泛型化請走 plugin 版。
- 圖同時存在 `core-diagram.txt` 與 `CONTEXT.md §8`,內容一致;修圖需兩處同步(`core-diagram.txt` 為來源,§8 為嵌入副本)。
