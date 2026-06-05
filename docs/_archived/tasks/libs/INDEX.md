# OS32 ユーザー空間ライブラリ設計書 索引

OS32 のユーザー空間ライブラリ (`programs/libos32*/`) の
設計書一覧。すべて `build/libs.mk` に登録されたアクティブなビルド対象。

> **Note:** 設計書が存在するライブラリについては、実装完了済みのため `_archived/libs/` に移動しました。
> 設計の参照が必要な場合はアーカイブ先を参照してください。

---

## 整合性監査 (2026-06-05)

`docs/libs/` 配下の全設計書を横断監査した結果と修正計画。

| ドキュメント | 内容 |
|-------------|------|
| [00_DOC_AUDIT.md](00_DOC_AUDIT.md) | 調査結果 — 依存関係コンフリクト・整合性・機能重複の検出一覧 (15 件) |
| [01_DOC_FIX_PLAN.md](01_DOC_FIX_PLAN.md) | 修正計画 — フェーズ別の対応・依存グラフ正本案・変更ファイルマトリクス |

---

## コア / 基盤ライブラリ

| ライブラリ | 設計書・ドキュメント | 実装パス | 概要 |
|-----------|--------------------|---------|------|
| libos32 | [README.md](../../libs/libos32/README.md) | `programs/libos32/` | OS32標準システムコールAPIラッパー、スタートアップ等 |
| libos32math | [README.md](../../libs/libos32math/README.md) / [DESIGN.md](../../libs/libos32math/DESIGN.md) | `programs/libos32math/` | FPU不要の整数数学ライブラリ (固定小数点, LUT, ベクトル) |
| libos32gfx | [README.md](../../libs/libos32gfx/README.md) | `programs/libos32gfx/` | グラフィックス描画・フォント・ジオメトリ等（ASM最適化含む） |
| libos32input | [README.md](../../libs/libos32input/README.md) / [DESIGN.md](../../libs/libos32input/DESIGN.md) | `programs/libos32input/` | 入力抽象化 (キーボード/マウス → アクションバインディング) |
| libos32asset | [README.md](../../libs/libos32asset/README.md) / [DESIGN.md](../../libs/libos32asset/DESIGN.md) | `programs/libos32asset/` | アセット・リソース管理 (ロード/破棄ライフサイクル) |
| libos32text | [README.md](../../libs/libos32text/README.md) / [DESIGN.md](../../libs/libos32text/DESIGN.md) | `programs/libos32text/` | RPG/ADV向けテキスト管理エンジン |

## シミュレーション・ロジック / データ

| ライブラリ | 設計書・ドキュメント | 実装パス | 概要 |
|-----------|--------------------|---------|------|
| libos32db | [README.md](../../libs/libos32db/README.md) | `programs/libos32db/` | SQLite データベース連携ライブラリ |
| libos32chem | [README.md](../../libs/libos32chem/README.md) / [DESIGN.md](../../libs/libos32chem/DESIGN.md) | `programs/libos32chem/` | BotW型化学エンジン (SQLite連携) |
| libos32map | [README.md](../../libs/libos32map/README.md) | `programs/libos32map/` | マップ管理・データベース連携ライブラリ |
| libos32econ | [README.md](../../libs/libos32econ/README.md) / [DESIGN.md](../../libs/libos32econ/DESIGN.md) | `programs/libos32econ/` | ターン制データ駆動型経済シミュレーション |
| ↳ estate拡張 | [ESTATE_DESIGN.md](../../libs/libos32econ/ESTATE_DESIGN.md) | 同上 | 不動産サブシステム拡張 |
| libos32ecs | [README.md](../../libs/libos32ecs/README.md) / [DESIGN.md](../../libs/libos32ecs/DESIGN.md) | `programs/libos32ecs/` | ゲームオブジェクト管理 (ECSパターン) |
| libos32event | [README.md](../../libs/libos32event/README.md) / [DESIGN.md](../../libs/libos32event/DESIGN.md) | `programs/libos32event/` | イベントスケジューラ (ターン/週/条件/確率) |

## バトル・ゲームシステム / 意思決定

| ライブラリ | 設計書・ドキュメント | 実装パス | 概要 |
|-----------|--------------------|---------|------|
| libos32ai | [README.md](../../libs/libos32ai/README.md) / [DESIGN.md](../../libs/libos32ai/DESIGN.md) | `programs/libos32ai/` | スコアベース汎用AI意思決定エンジン |
| libos32battle | [README.md](../../libs/libos32battle/README.md) / [DESIGN.md](../../libs/libos32battle/DESIGN.md) | `programs/libos32battle/` | ターンバトル解決エンジン |
| libos32board | [README.md](../../libs/libos32board/README.md) / [DESIGN.md](../../libs/libos32board/DESIGN.md) | `programs/libos32board/` | ノードグラフ型ボードゲームエンジン |
| libos32inv | [README.md](../../libs/libos32inv/README.md) / [DESIGN.md](../../libs/libos32inv/DESIGN.md) | `programs/libos32inv/` | インベントリ・装備・ショップエンジン |

## グラフィックス & UI / アプリケーション

| ライブラリ | 設計書・ドキュメント | 実装パス | 概要 |
|-----------|--------------------|---------|------|
| libos32tilemap | [README.md](../../libs/libos32tilemap/README.md) / [DESIGN.md](../../libs/libos32tilemap/DESIGN.md) / [INDEX](../../libs/libos32tilemap/INDEX.md) | `programs/libos32tilemap/` | SFC風4枚BGプレーン タイルマップ合成エンジン |
| libos32ui | [README.md](../../libs/libos32ui/README.md) | `programs/libos32ui/` | microUI (rxi) のOS32移植版 |
| libos32filer | [README.md](../../libs/libos32filer/README.md) | `programs/libos32filer/` | GFXファイラーライブラリ ＆ TVRAM描画 |

## メディア / ドキュメント

| ライブラリ | 設計書・ドキュメント | 実装パス | 概要 |
|-----------|--------------------|---------|------|
| libos32snd | [README.md](../../libs/libos32snd/README.md) | `programs/libos32snd/` | FM音源等サウンドドライバ連携 |
| libos32md | [README.md](../../libs/libos32md/README.md) | `programs/libos32md/` | Markdownパーサー ＆ レンダラー |

---

*Last Updated: 2026-05-23*
