# libos32map — マップ管理・データベース連携ライブラリ

## 1. 概要

`libos32map` は、SQLiteデータベース（`/db/map.db`など）から2Dマップデータをロードし、カメラの位置に基づいた描画範囲の決定、当たり判定（通行不可判定）、およびマップ上のイベント処理を行うライブラリです。

通常、画面合成エンジンである `libos32tilemap` と連携し、RPGやシミュレーションゲームの背景マップを管理します。

---

## 2. アーキテクチャ

本ライブラリは、マップの静的構造（レイアウト、タイル属性）をデータベースから読み込んでメモリ（RAM）に部分的にキャッシュし、キャラクター移動やカメラ制御に合わせて `libos32tilemap` へのタイル反映を行います。

```
  /db/map.db (SQLite)
      ↓
  [libos32map] ──(カメラ位置計算/当たり判定)
      │
      └── (描画タイルの転送) ──→ [libos32tilemap] ──→ 画面描画
```

---

## 3. 主要API

### 3.1 システム管理
*   `int map_init(const char *db_path)`
    *   マップデータベースを開き、初期設定を行います。
*   `void map_shutdown()`
    *   データベース接続を閉じ、確保したキャッシュメモリを解放します。

### 3.2 マップのロードと制御
*   `int map_load(u16 map_id)`
    *   指定したIDのマップデータをデータベースからロードし、現在のマップとして設定します。
    *   *(注: 将来的には `libos32asset` の Phase 3 集約計画に基づき、本関数の内部 I/O およびキャッシュ処理が [libos32asset](../libos32asset/DESIGN.md) 経由に統合される予定です, MIN-3)*
*   `const MapDef *map_current()`
    *   現在ロードされているマップの構造体（ID、名前、幅、高さなど）を取得します。

### 3.3 カメラ制御
*   `void map_set_camera(i16 x, i16 y)`
    *   マップの指定座標（タイル単位）にカメラの中心を移動させます。
*   `void map_get_camera(i16 *out_x, i16 *out_y)`
    *   現在のカメラ座標を取得します。

### 3.4 タイルマップ合成連携
*   `void map_apply_to_tilemap(int bg_layer, int force_redraw)`
    *   現在のカメラ位置に基づいて、指定したBGレイヤー（`libos32tilemap` のBGプレーン）に対応するタイルデータを転送し、スクロールを反映させます。

### 3.5 当たり判定・タイル情報
*   `int map_is_passable(i16 x, i16 y)`
    *   指定座標が移動可能かどうかを判定します（タイルの衝突属性）。
*   `u16 map_get_tile(i16 x, i16 y, int layer)`
    *   指定座標のタイルIDを取得します。

### 3.6 マップイベント・トリガー
*   `int map_check_trigger(i16 x, i16 y, u8 trigger_type, MapEvent *out_event)`
    *   指定座標で発生するイベント（ワープ、宝箱、会話など）を検索・取得します。

---

## 4. 使用例

基本的な初期化とマップ描画への適用手順：

```c
#include "libos32map.h"
#include "libos32tilemap.h"

void init_game_map(void) {
    /* タイルマップエンジンとマップエンジンの初期化 */
    tilemap_init(kapi);
    map_init("/db/map.db");

    /* タイルセットグラフィックを定義... */
    // tilemap_define(0, tile_grass_pixels);

    /* マップID 1をロード */
    map_load(1);

    /* カメラ初期位置を設定 */
    map_set_camera(10, 10);

    /* ロードされたデータを tilemap BG0 に適用 */
    map_apply_to_tilemap(0, 1);

    /* 描画・更新ループ */
    tilemap_compose_btf();
    tilemap_present();
}
```
