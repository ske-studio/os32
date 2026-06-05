# OS32 ユーザー空間ライブラリ 依存関係定義（正本）

*策定: 2026-06-05*

> [!IMPORTANT]
> **依存関係の表記凡例**
> `A → B` : A は B に依存する（A は B を利用する / B は下流で A より先にビルドされる必要がある）

OS32 のすべてのユーザー空間ライブラリは、循環依存のない一方向の DAG（有向非巡回グラフ）を形成するように設計されています。

---

## 1. 依存関係グラフ (層別)

```
L0 基盤レイヤー (外部依存なし、または KernelAPI のみ)
    ├── libos32math      (依存なし — 最も基底)
    ├── libos32db        (→ KAPI)
    ├── libos32          (→ KAPI ; crt0 / システムコールラッパー)
    ├── libos32input     (→ libos32math, KAPI)
    └── libos32asset     (→ KAPI)

L1 描画 / I-O 処理レイヤー
    ├── libos32gfx       (→ libos32math, KAPI)
    ├── libos32snd       (→ libos32math, KAPI)
    ├── libos32tilemap   (→ libos32math, libos32gfx)
    ├── libos32text      (→ libos32db)
    └── libos32map       (→ libos32db, libos32tilemap)

L2 シミュレーション / ゲームロジックレイヤー
    ├── libos32chem      (→ libos32math, libos32db)
    ├── libos32econ      (→ libos32math, libos32db)
    ├── libos32ai        (→ libos32math, libos32db)
    ├── libos32inv       (→ libos32db, libos32ai)
    └── libos32ecs       (→ libos32math)

L3 ゲームシステム解決レイヤー
    ├── libos32battle    (→ libos32math, libos32db, libos32ai)
    ├── libos32board     (→ libos32db)
    └── libos32event     (→ libos32db, libos32ai)

L4 アプリケーション / UI レイヤー
    ├── libos32ui        (→ libos32gfx)
    ├── libos32filer     (→ libos32gfx, KAPI)
    └── libos32md        (→ libos32gfx)
```

### 主要な依存関係のルールと設計判断
1. **ai ↔ board 循環依存の解消 (DEP-1)**
   - `libos32board` から `libos32ai` への依存はありません。また `libos32ai` から `libos32board` への直接的な依存もありません。
   - ボード上のマス情報と AI の意思決定は、上位の「ゲーム本体」が仲介して連携を行います。
2. **ecs ↔ asset 依存関係の整理 (DEP-2)**
   - `libos32asset` は L0 基盤レイヤーであり、`libos32ecs` には依存しません。
   - `libos32ecs` 自体は `libos32asset` や `libos32chem` などを直接の下流として依存するのではなく、ゲーム側でこれらを協調させるための「協調レイヤー」として機能します。
3. **ecs の input 依存の排除 (DEP-5)**
   - `libos32ecs` コアは入力機能（`libos32input`）に直接依存しません。
   - プレイヤー入力はゲーム側 System が `libos32input` から値を取得して Component に書き込む形態を取ります。

---

## 2. 各ライブラリの直接依存リスト

| ライブラリ | 直接依存先 | 備考 |
|---|---|---|
| **libos32** | KernelAPI | スタートアップ・システムコールラッパー |
| **libos32math** | なし | 整数演算、最も基底 |
| **libos32db** | KernelAPI | SQLite 接続管理 |
| **libos32input** | libos32math, KernelAPI | 入力抽象化 |
| **libos32asset** | KernelAPI | アセットローダー |
| **libos32gfx** | libos32math, KernelAPI | グラフィックス描画 |
| **libos32snd** | libos32math, KernelAPI | サウンド再生 |
| **libos32tilemap** | libos32math, libos32gfx | タイルマップ合成 |
| **libos32text** | libos32db | テキストエンジン |
| **libos32map** | libos32db, libos32tilemap | マップ管理 |
| **libos32chem** | libos32math, libos32db | 化学エンジン |
| **libos32econ** | libos32math, libos32db | 経済シミュレーション |
| **libos32ai** | libos32math, libos32db | AI 意思決定 |
| **libos32inv** | libos32db, libos32ai | インベントリ、ショップ |
| **libos32ecs** | libos32math | エンティティコンポーネントシステム |
| **libos32battle** | libos32math, libos32db, libos32ai | ターン制バトル |
| **libos32board** | libos32db | ボードゲームロジック |
| **libos32event** | libos32db, libos32ai | イベントスケジューラ |
| **libos32ui** | libos32gfx | microUI 移植版 GUI |
| **libos32filer** | libos32gfx, KernelAPI | ファイラーUI |
| **libos32md** | libos32gfx | Markdown レンダラー |
