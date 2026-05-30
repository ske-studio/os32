# OS32 ユーザー空間ライブラリ設計書 索引

OS32 のユーザー空間ライブラリ (`programs/libos32*/`) の
設計書一覧。すべて `build/libs.mk` に登録されたアクティブなビルド対象。

> **Note:** 設計書が存在するライブラリについては、実装完了済みのため `_archived/libs/` に移動しました。
> 設計の参照が必要な場合はアーカイブ先を参照してください。

---

## コア / 基盤ライブラリ

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32 | なし | `programs/libos32/` | OS32標準システムコールAPIラッパー、スタートアップ等 |
| libos32math | [LIBMATH_DESIGN.md](../_archived/libs/libmath/LIBMATH_DESIGN.md) | `programs/libos32math/` | FPU不要の整数数学ライブラリ (固定小数点, LUT, ベクトル) |
| libos32gfx | なし | `programs/libos32gfx/` | グラフィックス描画・フォント・ジオメトリ等（ASM最適化含む） |
| libos32input | [LIBINPUT_DESIGN.md](../_archived/libs/libinput/LIBINPUT_DESIGN.md) | `programs/libos32input/` | 入力抽象化 (キーボード/マウス → アクションバインディング) |
| libos32asset | [LIBASSET_DESIGN.md](../_archived/libs/libasset/LIBASSET_DESIGN.md) | `programs/libos32asset/` | アセット・リソース管理 (ロード/破棄ライフサイクル) |
| libos32text | [LIBTEXT_DESIGN.md](../_archived/libs/libtext/LIBTEXT_DESIGN.md) | `programs/libos32text/` | RPG/ADV向けテキスト管理エンジン |

## シミュレーション・ロジック / データ

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32db | なし（[SQLITE_INTEGRATION.md](../../SQLITE_INTEGRATION.md)） | `programs/libos32db/` | SQLite データベース連携ライブラリ |
| libos32chem | [LIBCHEM_DESIGN.md](../_archived/libs/libchem/LIBCHEM_DESIGN.md) | `programs/libos32chem/` | BotW型化学エンジン (SQLite連携) |
| libos32map | なし | `programs/libos32map/` | マップ管理・データベース連携ライブラリ |
| libos32econ | [LIBECON_DESIGN.md](../_archived/libs/libecon/LIBECON_DESIGN.md) | `programs/libos32econ/` | ターン制データ駆動型経済シミュレーション |
| ↳ estate拡張 | [LIBECON_ESTATE_DESIGN.md](../_archived/libs/libecon/LIBECON_ESTATE_DESIGN.md) | 同上 | 不動産サブシステム拡張 |
| libos32ecs | [LIBECS_DESIGN.md](../_archived/libs/libecs/LIBECS_DESIGN.md) | `programs/libos32ecs/` | ゲームオブジェクト管理 (ECSパターン) |
| libos32event | [LIBEVENT_DESIGN.md](../_archived/libs/libevent/LIBEVENT_DESIGN.md) | `programs/libos32event/` | イベントスケジューラ (ターン/週/条件/確率) |

## バトル・ゲームシステム / 意思決定

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32ai | [LIBAI_DESIGN.md](../_archived/libs/libai/LIBAI_DESIGN.md) | `programs/libos32ai/` | スコアベース汎用AI意思決定エンジン |
| libos32battle | [LIBBATTLE_DESIGN.md](../_archived/libs/libbattle/LIBBATTLE_DESIGN.md) | `programs/libos32battle/` | ターンバトル解決エンジン |
| libos32board | [LIBBOARD_DESIGN.md](../_archived/libs/libboard/LIBBOARD_DESIGN.md) | `programs/libos32board/` | ノードグラフ型ボードゲームエンジン |
| libos32inv | [LIBINV_DESIGN.md](../_archived/libs/libinv/LIBINV_DESIGN.md) | `programs/libos32inv/` | インベントリ・装備・ショップエンジン |

## グラフィックス & UI / アプリケーション

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32tilemap | [01_TILEMAP_DESIGN.md](../_archived/tilemap/01_TILEMAP_DESIGN.md) | `programs/libos32tilemap/` | SFC風4枚BGプレーン タイルマップ合成エンジン |
| libos32ui | なし | `programs/libos32ui/` | microUI (rxi) のOS32移植版 |
| libos32filer | なし | `programs/libos32filer/` | GFXファイラーライブラリ ＆ TVRAM描画 |

## メディア / ドキュメント

| ライブラリ | 設計書 | 実装パス | 概要 |
|-----------|--------|---------|------|
| libos32snd | なし | `programs/libos32snd/` | FM音源等サウンドドライバ連携 |
| libos32md | なし | `programs/libos32md/` | Markdownパーサー ＆ レンダラー |

---

*Last Updated: 2026-05-23*
