# libos32snd — サウンド＆FM音源ライブラリ

## 1. 概要

`libos32snd` は、PC-9801/9821に搭載されているFM音源ボード（PC-9801-26Kや86音源等）と連携し、BGM（MML形式）の再生制御および効果音（SE）の発音を行うためのライブラリです。

OS32カーネル内のFMサウンドドライバに対するKernelAPIインターフェースをラップし、ゲーム等の外部プログラムから簡単な呼び出しでマルチチャネル音源再生を実現します。

---

## 2. アーキテクチャ

カーネル側にサウンドミキサー・シーケンサ（FM/SSG音源コントローラ）が組み込まれており、本ライブラリはこれを KernelAPI を介して制御します。

```
  [アプリケーション] (MMLデータの指定 / 発音要求)
         ↓
  [libos32snd] (APIラッパー)
         ↓
   KernelAPI (sys_snd_play / sys_snd_stop / sys_snd_command)
         ↓
  [OS32カーネル サウンドドライバ] (I/O制御・割込タイミング)
         ↓
  FM音源ボード (YM2203 / YM2608 音源チップ等) ──→ 音声出力
```

---

## 3. 主要API

### 3.1 システム制御
*   `int snd_init(KernelAPI *api)`
    *   サウンドシステムを初期化し、FM音源デバイスの存在を確認します。
*   `void snd_shutdown()`
    *   サウンド演奏を停止し、音源チップをミュート状態にしてシステムを終了します。

### 3.2 BGMの再生と制御
*   `int snd_play_bgm(const char *mml_data)`
    *   指定したMML形式のBGMテキストデータを再生します。
    *   *(注: 将来的には `libos32asset` の Phase 3 集約計画に基づき、MMLデータのファイルロードおよびキャッシュ処理が [libos32asset](../libos32asset/DESIGN.md) 経由に統合される予定です, MIN-3)*
*   `void snd_stop_bgm(void)`
    *   現在演奏中のBGMを停止します。
*   `void snd_pause_bgm(void)` / `void snd_resume_bgm(void)`
    *   BGM演奏の一時停止および再開。
*   `int snd_is_bgm_playing(void)`
    *   BGMが現在再生中かどうかを判定します。

### 3.3 効果音（SE）の制御
*   `void snd_play_se(int se_id, int priority)`
    *   指定した効果音ID（事前に定義または登録されたSEパターン）を再生します。複数のSEが競合した場合は優先度（`priority`）に基づいて割り込みまたは並行発音を処理します。

### 3.4 音源コマンド（直接制御）
*   `void snd_write_reg(u8 port, u8 reg, u8 val)`
    *   FM音源チップ（YM2203/YM2608など）のレジスタへ直接データを書き込みます。独自のエフェクトや発音制御を行う場合に使用します。

---

## 4. 特徴と設計判断

*   **MMLテキストのサポート**: 標準的なPC-98音楽ソフトに類似したMML（Music Macro Language）形式での記述をサポートしています。
*   **非ブロッキング発音**: BGMシーケンスの進行はカーネル側のタイマー割り込み（PIT割り込み）によって非同期に駆動されるため、ユーザー空間のゲーム処理（60fpsループ等）をブロックしません。
