# gRPC Header Kit — 解決方案摘要

> 內部解決方案報告 · 對象:管理層 + 工程師 · 提供:Receiver Team

## TL;DR

在既有 30 個 tx 之上加一層 **routing header,從 payload 自動衍生**,讓 APISIX 不 parse payload 即可路由。一致性靠「header 與 body 同源」保證,非人工同步。

## 問題

- 既有 gRPC interface:**4 services / 30 tx methods**,payload 已生成。
- 缺 **header 供 APISIX context-based routing**;目前路由需 parse payload,耦合深、成本高。

## 約束

- **Comply gRPC**:metadata key 小寫、value 限可印 ASCII → value 一律 URL-encode。
- **Metadata ≤ 8 KB**(硬上限)。

## 核心機制

映射綁在 proto 欄位的 custom option 上,header 從**同一個 message 實例**自動生成。

```
proto 欄位 [(routing.header)]
        │   body = SSOT
        ▼
   同一個 message 實例
    ├─→ body     (既有 payload,不動)
    └─→ headers  (本 kit 自動生成,讀同一棵 tree)
```

body 與 header 讀同一棵 message tree → 結構上無法「body 有、header 漏」。其餘需求皆為此機制的衍生:

- APISIX 純 header routing,不碰 payload(header = payload 投影子集)
- 改版時 sender 不 hardcode(改 proto,header 自動跟隨)
- 易擴充(option 可加 group / 欄位,向後相容)

## Domain Context 映射

| Header | Cardinality | 對應 |
|---|---|---|
| `tool-header` | 1(required) | 機台 / 腔體層級 |
| `lot-header` | 1..N | 每個 Lot 一筆 |
| `mask-header` | 0..N | 每個 Mask 一筆 |

- 格式:URL-encoded `key=value&key=value`。
- 多筆 = gRPC repeated metadata(同名 key 多筆),不用 index 後綴。

## Cardinality × 8 KB:已驗證

最壞情境 **1 tx = 25 lots + 3 masks**:

| 項目 | 值 |
|---|---|
| 總用量(tool ×1 + lot ×25 + mask ×3) | ≈ 1.5 KB |
| 8 KB 預算使用率 | ~19% |
| 餘裕 | ~81%(6.6 KB) |
| 理論觸頂 | ~127 筆 lot |

結論:真實 cardinality 下逐筆列舉安全,**不需退化策略**。

## Interface 三件套

| 件 | 內容 | 來源 |
|---|---|---|
| body | 既有 payload | sender(不動) |
| headers | 三個 routing header | 本 kit 從 proto option 自動生成 |
| sample code | reflection(入門)/ plugin codegen(production) | 本 kit |

- plugin codegen 把「漏改 header」擋在**編譯期**。
- 已端到端驗證(protobuf 3.21.12):生成 code 編得過、輸出 byte-identical;非 scalar 欄位標 option 在 build 時失敗報錯。

## 治理

- Spec 進 **repo 管控**;`header_options.proto` = 契約 SSOT,版本化。
- Sender 只 `import`,不複製規則。
- 改 group 語意 = breaking change → bump 版本 + 通知。

## 待拍板(非阻斷)

- APISIX 對 repeated header 的消費方式(逐筆 vs 逗號合併)— 需實測;sender 暫按「逐筆 repeated」行事。
- 8 KB 為保守上限假設,gateway 實際設定待確認(不影響上述容量結論)。

---

詳見:`REQUIREMENTS.md`(逐條 spec)· `PROPOSAL.md`(用法)· `CONTEXT.md`(決策脈絡)
