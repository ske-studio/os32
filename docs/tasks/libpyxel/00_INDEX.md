# libpyxel — OS32 Pyxel互換ゲームエンジン ドキュメント索引

OS32上でPyxel互換のゲームエンジンを構築するための技術資料集。

## ドキュメント一覧

| No. | ファイル | 内容 |
|-----|---------|------|
| 01 | [PYXEL_FORMAT.md](01_PYXEL_FORMAT.md) | `.pyxres` リソースファイルフォーマット解析 (公式 pyxres-format.md 準拠) |
| 02 | [VRAM_PLANAR.md](02_VRAM_PLANAR.md) | PC-9801 VRAMプレーン方式とパックトピクセル変換の設計 |
| 03 | [SCALING.md](03_SCALING.md) | 256×192 → 512×384 (2倍) スケーリング性能検証と設計 |
| 04 | [API_MAPPING.md](04_API_MAPPING.md) | Pyxel API → OS32 C API マッピング仕様 |
| 05 | [ROADMAP.md](05_ROADMAP.md) | 実装ロードマップ (フェーズ計画) |
| 06 | [IMPLEMENTATION_DETAILS.md](06_IMPLEMENTATION_DETAILS.md) | libpyxel APIリファレンスとモジュール・ディレクトリ構成 |
| 07 | [BENCH_RESULTS.md](07_BENCH_RESULTS.md) | Phase 0 スケーリング性能ベンチマーク結果と最適描画方式の選定 |

## 基本方針

- **libpyxel は libos32gfx 上に構築されるラッパーレイヤー**である
  - 独自の描画実装は持たず、libos32gfx のプリミティブ・スプライト・サーフェス機能に委譲する
  - 2倍座標変換 (256×192 → 512×384) は libos32gfx 側にスケーリング描画関数として実装し、libpyxel はそれをラップして Pyxel 互換 API を提供する
- Pyxel Editorで作成した `.pyxres` データをホスト側Pythonスクリプトで変換し、OS32で利用可能にする
- 内部解像度 **256×192**、画面表示は **2倍拡大 (512×384)**
- 残りの画面領域 (右側128px、下部16px) はUI/デバッグ情報に使用
- 描画はPC-9801のVRAMプレーン方式に合わせた設計とする
- OS32ではPyxelの **カスタム16色パレット** をサポート (初期値はPyxelデフォルトパレット、アプリから変更可能)

## アーキテクチャ

```text
┌──────────────────────────────────────────────┐
│  アプリケーション (Pyxel互換ゲームコード)      │
├──────────────────────────────────────────────┤
│  libpyxel (Pyxel互換ラッパーレイヤー)         │
│  ├─ pyxel_core.c   : init/run/quit/present   │
│  ├─ pyxel_gfx.c    : cls/pset/line/rect/circ  │
│  │   → libos32gfx のスケーリング描画APIをラップ │
│  ├─ pyxel_input.c  : btn/btnp (KAPI経由)      │
│  ├─ pyxel_snd.c    : play/playm (KAPI経由)    │
│  └─ pyxel_res.c    : .os32res ロード          │
├──────────────────────────────────────────────┤
│  libos32gfx (ユーザーランドGFXライブラリ)      │
│  ├─ Primitives : rect/circle/line/pixel 等    │
│  ├─ Scaled API : 2倍座標変換付き描画関数     │
│  ├─ Surface    : オフスクリーン描画面          │
│  ├─ Sprite     : 事前レンダリング+高速ブリット │
│  └─ Font/KCG   : テキスト描画                 │
├──────────────────────────────────────────────┤
│  KAPI (カーネルAPI)                           │
│  ├─ gfx_get_framebuffer : BB直接アクセス       │
│  ├─ gfx_present_dirty   : VRAM転送            │
│  ├─ kbd_is_pressed      : キー状態             │
│  └─ snd_*               : サウンド制御         │
└──────────────────────────────────────────────┘
```

## 公式リファレンス

- [Pyxel GitHub (kitao/pyxel)](https://github.com/kitao/pyxel) — 最新 v2.9.0, Rust+Python
- [Pyxel pyxres-format.md](https://github.com/kitao/pyxel/blob/main/docs/pyxres-format.md) — 公式リソースフォーマット仕様
- [Pyxel User Guide](https://kitao.github.io/pyxel/web/user-guide/) — ユーザーガイド
- [Pyxel API Reference](https://kitao.github.io/pyxel/web/api-reference/) — APIリファレンス
