# タイルマップエンジン — ドキュメント索引

SFC風4プレーンBGタイルマップ合成エンジンの設計・実装ドキュメント。

## ドキュメント一覧

| No. | ファイル | 内容 |
|-----|---------|------|
| 01 | [TILEMAP_DESIGN.md](01_TILEMAP_DESIGN.md) | タイルマップエンジン設計仕様 |
| 02 | [BLIT_COLORKEY_OPT.md](02_BLIT_COLORKEY_OPT.md) | gfx_blit_colorkey 高速化リファクタリング |

## コンセプト

- 16×16タイル × 24×24グリッド = 384×384 論理解像度
- 4枚のBGプレーン（BG0=最背面 〜 BG3=最前面）
- **Back-to-Front** と **Front-to-Back** の2方式を提供し、ユーザーが選択可能
- 既存GFXインフラ（`gfx_get_framebuffer`, `gfx_blit`, `gfx_add_dirty_rect`, `gfx_present_dirty`）を最大限活用
- 16色パレット共有はハードウェア制約として受容
