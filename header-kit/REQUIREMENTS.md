# REQUIREMENTS — gRPC Header Kit

> 規格文件 · 逐條可追蹤 · 配套:SUMMARY.md(摘要)、PROPOSAL.md(用法)、CONTEXT.md(決策)

ID 規則:FR = 功能需求,NFR = 非功能需求 / 約束,CON = 設計約定。每條附驗收條件(AC)。

---

## 1. 範圍

- **In**:從既有 proto message 衍生三個 gRPC metadata header,供 APISIX context-based routing。
- **Out**:body(XML payload)序列化、APISIX route 規則本身、RPC 傳輸。
- **既有資產**:4 services / 30 tx methods / payload 已生成,本 kit 只新增 header 層。

---

## 2. 功能需求(FR)

### FR-1 Header 自動生成
- header 從 proto message 實例衍生;sender 不手寫 header、不維護外部映射檔。
- **AC**:修改 proto 欄位後重新 codegen,header 輸出隨之改變,sender 業務碼不需更動。

### FR-2 三個 Domain Context Header
- `tool-header` — cardinality **1**,required。
- `lot-header` — cardinality **1..N**。
- `mask-header` — cardinality **0..N**。
- **AC**:無 tool 欄位 → 報錯;無 lot → 報錯;無 mask → 合法(略過)。

### FR-3 Cardinality 表達
- 多筆以 gRPC **repeated metadata**(同名 key 多筆)表達,不使用 index 後綴(`lot-header-0/1`)。
- **AC**:25 個 lot 產生 25 筆同名 `lot-header`。

### FR-4 APISIX Header Routing
- routing 僅依 header 欄位,gateway 不 parse payload。
- **AC**:移除 payload 內容仍可正確路由(僅憑 header)。

### FR-5 Header 內容為 Payload 投影
- header 欄位值必為 body 已有之值;header 不得攜帶 body 不存在的資訊。
- **AC**:每個 header key=value 皆可在同一 message 找到對應欄位。

### FR-6 欄位選擇性標註
- 僅標註 `(routing.header)` 的欄位進 header;未標註欄位只進 body。
- **AC**:未標註欄位不出現在任何 header。

---

## 3. 非功能需求 / 約束(NFR)

### NFR-1 gRPC 合規
- metadata key 全小寫;value 限可印 ASCII(0x20–0x7E)。
- 實作:value 一律 URL-encode(RFC 3986;空白 `%20`,不用 `+`)。
- **AC**:含中文 / 特殊字元的欄位值經 encode 後皆為合法 ASCII metadata。

### NFR-2 Metadata 大小 ≤ 8 KB
- 單一 request 的 metadata 總量不得超過 8 KB(硬上限)。
- 已驗證:最壞 25 lots + 3 masks ≈ 1.5 KB(~19% 預算,~81% 餘裕);理論觸頂 ~127 筆 lot。
- **AC**:典型最壞 cardinality 之 header 總量 < 8 KB,無需退化策略。

### NFR-3 改版不 Hardcode
- interface 改版時,sender 僅 import 新版 proto,不需修改 header 生成碼。
- **AC**:proto 增 / 改標註欄位後,sender 重編即生效,無 hardcode 字串。

### NFR-4 Extensibility
- 機制需支援未來新增 header group 或欄位,且向後相容。
- **AC**:新增一個 group 不破壞既有 sender 的編譯與輸出。

### NFR-5 Strong Consistency
- body 與 header 不得漂移;漏改須在編譯期(plugin path)被攔截。
- **AC**:欄位標註於非 scalar → protoc build 失敗並報明確訊息。

### NFR-6 跨產品泛化
- 同一機制適用於數十個 tx,不為個別 tx 寫特例。
- **AC**:不同 tx message 套用同一 option + sample,無需 per-tx 客製邏輯。

---

## 4. 設計約定(CON)

### CON-1 Domain → Header 固定映射
- group `TOOL`/`LOT`/`MASK` 固定對應 `tool-header`/`lot-header`/`mask-header`;sender 不自填 metadata key 名。

### CON-2 僅 Scalar 進 Header
- 目前僅支援 scalar 欄位(string/int/bool 等);巢狀 message 不支援。
- 違反者於 plugin build 時失敗(見 NFR-5 AC)。

### CON-3 格式凍結
- URL-encode 規則上線後凍結;變更需 sender/receiver 協調(否則 decode 不一致)。

### CON-4 `order` 為 best-effort
- `order` 僅控制輸出順序,不影響正確性(receiver 以 key 查找)。proto3 `order: 0` 視同未填,從 1 起編。

### CON-5 Contract SSOT 於 Repo
- `header_options.proto` 為契約唯一來源,版本化;sender import 不複製。改 group 語意 = breaking change。

---

## 5. 追蹤矩陣(需求 → 設計/驗證)

| 需求 | 設計手段 | 驗證狀態 |
|---|---|---|
| FR-1 / NFR-3 / NFR-5 | proto custom option + codegen | ✅ plugin 端到端驗證 |
| FR-2 / FR-3 | repeated metadata + cardinality 檢查 | ✅ 25 lots 輸出比對 |
| FR-4 / FR-5 / FR-6 | header = scalar 投影子集 | ✅ 輸出 byte-identical |
| NFR-1 | 共用 url_encode.h | ✅ 單元測試(含 CJK) |
| NFR-2 | 容量估算 | ✅ 1.5 KB / 19% |
| NFR-4 / NFR-6 | group enum 可擴充 | ✅ 多層 package / 多 tx 適用 |
| CON-2 | 非 scalar build-time 攔截 | ✅ protoc 失敗報錯 |

---

## 6. 開放項(OPEN)

- **OPEN-1** APISIX 對 repeated header 的消費方式(逐筆 / 合併)+ gateway metadata 上限實際值。
- **OPEN-2** cardinality 上界正式確認(現以 25 lots + 3 masks 為設計最壞值)。
- **OPEN-3** 巢狀 message 進 header 之未來需求。
- **OPEN-4** extension number `50001` 登記 proto registry。
