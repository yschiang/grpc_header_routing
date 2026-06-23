# CONTEXT — gRPC Header Generation Kit

> 版本: v0.1 · 維護: Receiver Team
> 用途: 讓接手的人/AI agent 在不重讀全部對話的前提下,理解這套 header kit 的需求脈絡、為什麼是現在這個設計、哪些路被否決、有哪些硬約束。
> 配套文件: `PROPOSAL.md`(給 sender 的方案 + 用法)、`proto/header_options.proto`(契約)、兩支 sample。

---

## 1. 一句話定位

Receiver 端提供一套 **proto custom option + sample code**,讓 C++ sender 在用 `.proto` 產生 XML body 的同時,從**同一份 message 實例**衍生出三個 gRPC metadata header,供 **APISIX 做 content-based routing**。

---

## 2. 角色與邊界

| 角色 | 是誰 | 負責 |
|---|---|---|
| **Receiver** | 本團隊(本 kit 提供方) | 定義 option 契約、提供 sample、定 header 格式、收 request 後解析 header |
| **Sender** | C++ 服務團隊(外部使用者) | 標註自己的 `.proto`、產 body(XML)、發送時掛 header |
| **APISIX** | gateway | 依 header 做 routing(規則由 routing 設計決定,非本 kit) |

**本 kit 的邊界**:只負責「header 怎麼從 message 衍生」。**不負責** body(XML)序列化、不負責 APISIX route 規則本身、不負責 RPC 傳輸。

---

## 3. 原始需求(問題陳述)

1. Sender 用 `.proto` → 產 XML body 送 receiver。
2. APISIX 需要 routing,但不想 parse 整個 XML body(成本高、耦合深),改用 **gRPC metadata header** 做路由判斷。
3. 需要三個 header,cardinality 不同:
   - `tool-header`:**1**(required)
   - `lot-header`:**1..N**
   - `mask-header`:**0..N**(已確認可能多筆)
4. 每個 header 是 **URL-encoded、`&` 分隔的 `key=value` 串**(例:`EqpID=T01&ChamberId=C1`)。
5. 這些值 **body 裡也都有** —— 所以 header 是 body 的子集投影,不是新資訊。

**衍生出的核心需求**:header 與 body 必須一致。兩者若不同步,APISIX 會 route 到錯的地方,且錯誤是**靜默**的(傳輸成功、路由錯誤)。這是整個設計的第一驅動力。

---

## 4. 設計驅動力(為什麼是現在這個樣子)

| # | 驅動力 | 帶來的設計決定 |
|---|---|---|
| D1 | header 必一致、漏改要在**編譯期**被擋 | 映射標在 proto 欄位上(custom option),不另立映射檔 |
| D2 | sender 是 **C++ 單一語言**、發送可能高頻 | production 走 protoc plugin codegen(zero reflection) |
| D3 | 要降低 sender 上手門檻 | 同時給 reflection 入門 sample + plugin production sample |
| D4 | body 才是完整資料,header 只是投影 | **body = SSOT**;header 從同一 message tree 衍生 |
| D5 | routing 值含特殊字元 / 可能含中文 | URL-encode 為硬性要求(且滿足 metadata ASCII-only) |
| D6 | Lot/Mask 多筆 | 用 gRPC repeated metadata(同名 key 多筆),不做 index 後綴 |

---

## 5. 被否決的方案(及原因)

> 記錄這些是為了讓接手者不要重走一遍。

**A1. 額外維護一份映射檔(YAML/JSON 描述欄位→header)**
否決。等於開第二個 source,proto 改了映射檔可能忘改 → 正是 D1 要消滅的靜默漂移。違背 SSOT。

**A2. APISIX 直接 parse XML body 做 routing**
否決(需求前提)。成本高、gateway 與 body schema 強耦合,body 一改 routing 就可能壞。改用 header 投影解耦。

**A3. Runtime reflection 當 production 方案**
否決為 production,保留為入門 sample。C++ reflection 讀 option 冗長且慢、失去 type safety、每次發送付遍歷成本。高頻路徑不可接受(D2),故 production 用 codegen。

**A4. Lot 多筆用 `lot-header-0` / `lot-header-1` 後綴**
否決。gRPC metadata 原生支援同名 key 多值(repeated),後綴方案徒增 parse 複雜度且不符 HTTP/gRPC 慣例。

**A5. 把格式邏輯(encode/串接/cardinality)也塞進 option**
否決。option 只表達「意圖」(哪個 group、key 叫什麼),格式邏輯集中在 sample code,保持 proto 乾淨、格式可統一演進。

---

## 6. 硬約束(不可違反)

- **C1 metadata 大小上限**:gRPC/APISIX 對 request metadata 總量有上限(保守假設 8 KB,實際待 OPEN-1 確認)。Lot/Mask 多筆累加易撐爆 → 大量時需退化為「只帶共通維度」。
- **C2 metadata value 只允許可印 ASCII(0x20–0x7E)**(非 `-bin` key)→ URL-encode 為硬性要求,非美化。
- **C3 metadata key 全小寫**(gRPC 規範)→ `tool-header`/`lot-header`/`mask-header`,由 group 固定,sender 不自填。
- **C4 格式約定凍結**:URL-encode 規則(unreserved 不編、空白 `%20` 不用 `+`、key 與 value 都 encode)上線後凍結,變更需 sender/receiver 協調,否則 decode 錯。
- **C5 僅 scalar 進 header**:目前不支援巢狀 message,scalar(string/int/bool)為限。
- **C6 SSOT = body**:header 不得帶任何 body 沒有的資訊;header 永遠是 body 的投影。

---

## 7. 關鍵決策記錄(ADR 摘要)

**ADR-1 映射放 proto custom option,不放外部檔**
理由:唯一能在編譯期綁定欄位與 header、擋住漏改的方案(D1)。代價:需寫 protoc plugin 才能「使用」option(protoc 只「保存」option)。

**ADR-2 Mask 為 `0..N` 而非 `0..1`**
理由:確認實務上一個 request 可能對應多個 mask。實作與 Lot 同構(repeated metadata),降低特例。

**ADR-3 提供雙 sample(reflection + plugin)**
理由:reflection 驗證鏈路、降門檻;plugin 才是 production。明確標示 reflection 版的泛型遍歷限制,引導高頻路徑切 plugin。

**ADR-4 `order` 為 best-effort,不影響正確性**
理由:receiver 以 key 查找解析,不依賴順序;order 僅為輸出穩定/可讀。注意 proto3 `order: 0` 視同未填,編號從 1 起。

**ADR-5 extension number 50001 固定保留**
理由:作為長期契約,number 必須穩定且登記到 proto registry(OPEN-4),避免與其他 option 衝突。

---

## 8. 一致性如何被保證(設計的核心論證)

```
                          gRPC Header Kit — 核心解法
                   body = SSOT,header 從同一 message 衍生
═══════════════════════════════════════════════════════════════════════════════

  request.proto  (標註的來源 · SSOT)
  ┌─────────────────────────────────────────────────────────┐
  │ message Request {                                        │
  │   string eqp_id     = 1 [(routing.header)={TOOL,EqpID}]; │──┐ 意圖綁在
  │   string chamber_id = 2 [(routing.header)={TOOL,...}];   │──┤ 欄位旁
  │                                                          │  │
  │   repeated Lot  lots  = 3;   // → lot-header  1..N       │──┤
  │   repeated Mask masks = 4;   // → mask-header 0..N       │──┤
  │                                                          │  │
  │   string operator_id = 5;    // 沒標 → 只進 body         │  │ (不進 header)
  │ }                                                        │  │
  └─────────────────────────────────────────────────────────┘  │
                            │                                    │
                            │  讀同一個 message 實例 (1 × msg)   │
                            ▼                                    │
                     ┌────────────┐                             │
                     │  Request   │  ◀── annotation 在此被讀取 ──┘
                     │  實例      │
                     └─────┬──────┘
                  ┌────────┴─────────┐
                  │ 同一棵 tree 分岔 │
            ──────┴──────      ──────┴──────────────────
            ▼                  ▼
   ┌──────────────────┐   ┌──────────────────────────────────────────┐
   │ body · XML       │   │ gRPC metadata · header                   │
   │ [SENDER 自理]    │   │ [本 KIT 交付]                            │
   │ 不在 kit 範圍    │   │                                          │
   ├──────────────────┤   ├──────────────────────────────────────────┤
   │ <Request>        │   │ tool-header: EqpID=T01&ChamberId=C1       │
   │   <EqpID>T01<..> │   │ lot-header:  LotId=L001&Step=ST10&...     │
   │   <Lot ..>..<..> │   │ lot-header:  LotId=L002&Step=ST20         │
   │   ... 完整資料   │   │ mask-header: MaskId=M99&LayerId=METAL1    │
   │ </Request>       │   │                                          │
   └──────────────────┘   │ URL-encoded · 同名 key 多筆 = repeated   │
                          └──────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════
 為什麼不會漂移:
   [01] 意圖綁在欄位上    映射放 proto option,不放外部檔 → 改欄位不會漏改
   [02] 同一棵 message    body/header 讀同一實例 → header 是 body 的投影
   [03] 編譯期擋漏改      plugin codegen:改 proto → 重 codegen → header 自動跟上
```

因為 header 與 body 讀**同一棵 message tree**,且映射意圖就在欄位旁邊,結構上無法產生「body 有、header 漏」的漂移。plugin 版更進一步把這個保證推到**編譯期**:改 proto → 重 codegen → header builder 自動跟上,成為 build 的一環。

---

## 9. 未解決事項(OPEN,與 PROPOSAL §11 同步)

- **OPEN-1** APISIX 對 repeated header 的實際消費方式(逐筆 / 逗號合併 / 取首)+ metadata 大小上限實際數字。
- **OPEN-2** Lot/Mask 數量上界,與超限退化策略。
- **OPEN-3** 未來是否需要巢狀 message 進 header。
- **OPEN-4** extension number 50001 正式登記。

> 目前對 sender 的行事假設:routing 以**逐筆 repeated metadata** 消費,sender 不需處理合併邏輯(待 OPEN-1 拍板)。

---

## 10. 詞彙

| 詞 | 意思 |
|---|---|
| SSOT | Single Source of Truth;此處指 body(message 實例) |
| custom option | proto `FieldOptions` 擴充,標在欄位上的 metadata |
| repeated metadata | gRPC 同名 metadata key 帶多筆 value |
| codegen | build time 由 protoc plugin 生成 code(對比 runtime reflection) |
| cardinality | 數量約束(1 / 1..N / 0..N) |
