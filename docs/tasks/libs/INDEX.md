# OS32 ユーザー空間ライブラリ設計書 索引

OS32 のユーザー空間ライブラリ (`programs/libos32*/` および `programs/libtilemap/`) の
設計書一覧。すべて `build/libs.mk` に登録されたアクティブなビルド対象。

---

## 基盤ライブラリ

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32math | [LIBMATH_DESIGN.md](libmath/LIBMATH_DESIGN.md) | `programs/libos32math/` | FPU不要の整数数学ライブラリ (固定小数点, LUT, ベクトル) |
| libos32input | [LIBINPUT_DESIGN.md](libinput/LIBINPUT_DESIGN.md) | `programs/libos32input/` | 入力抽象化 (キーボード/マウス → アクションバインディング) |
| libos32asset | [LIBASSET_DESIGN.md](libasset/LIBASSET_DESIGN.md) | `programs/libos32asset/` | アセット・リソース管理 (ロード/破棄ライフサイクル) |
| libos32text | [LIBTEXT_DESIGN.md](libtext/LIBTEXT_DESIGN.md) | `programs/libos32text/` | RPG/ADV向けテキスト管理エンジン |

## シミュレーション・ロジック

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32chem | [LIBCHEM_DESIGN.md](libchem/LIBCHEM_DESIGN.md) | `programs/libos32chem/` | BotW型化学エンジン (SQLite連携) |
| libos32econ | [LIBECON_DESIGN.md](libecon/LIBECON_DESIGN.md) | `programs/libos32econ/` | ターン制データ駆動型経済シミュレーション |
| ↳ estate拡張 | [LIBECON_ESTATE_DESIGN.md](libecon/LIBECON_ESTATE_DESIGN.md) | 同上 | 不動産サブシステム拡張 |
| libos32ecs | [LIBECS_DESIGN.md](libecs/LIBECS_DESIGN.md) | `programs/libos32ecs/` | ゲームオブジェクト管理 (ECSパターン) |
| libos32event | [LIBEVENT_DESIGN.md](libevent/LIBEVENT_DESIGN.md) | `programs/libos32event/` | イベントスケジューラ (ターン/週/条件/確率) |

## バトル・ゲームシステム

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32ai | [LIBAI_DESIGN.md](libai/LIBAI_DESIGN.md) | `programs/libos32ai/` | スコアベース汎用AI意思決定エンジン |
| libos32battle | [LIBBATTLE_DESIGN.md](libbattle/LIBBATTLE_DESIGN.md) | `programs/libos32battle/` | ターンバトル解決エンジン |
| libos32board | [LIBBOARD_DESIGN.md](libboard/LIBBOARD_DESIGN.md) | `programs/libos32board/` | ノードグラフ型ボードゲームエンジン |
| libos32inv | [LIBINV_DESIGN.md](libinv/LIBINV_DESIGN.md) | `programs/libos32inv/` | インベントリ・装備・ショップエンジン |

## グラフィックス

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libtilemap | [01_TILEMAP_DESIGN.md](tilemap/01_TILEMAP_DESIGN.md) | `programs/libtilemap/` | SFC風4枚BGプレーン タイルマップ合成エンジン |

### tilemap 関連ドキュメント

| ファイル | 内容 |
|---------|------|
| [00_INDEX.md](tilemap/00_INDEX.md) | tilemap ドキュメント索引 |
| [02_BLIT_COLORKEY_OPT.md](tilemap/02_BLIT_COLORKEY_OPT.md) | ブリット＋カラーキー最適化 |
| [03_ROTATE_BLIT.md](tilemap/03_ROTATE_BLIT.md) | 回転ブリット |
| [04_SCROLL_OPT.md](tilemap/04_SCROLL_OPT.md) | スクロール最適化 |
| [05_SCROLL_ASM_OPT.md](tilemap/05_SCROLL_ASM_OPT.md) | スクロール ASM 最適化 |
| [06_REFACTOR_PLAN.md](tilemap/06_REFACTOR_PLAN.md) | リファクタリング計画 |
| [07_TODO.md](tilemap/07_TODO.md) | TODO リスト |

---

*Last Updated: 2026-05-04*
