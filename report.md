# rcmd.py シリアル通信 先頭文字ドロップ バグレポート

## 症状

`rcmd.py` 経由でリモートシェルにコマンドを送信すると、**先頭1文字が必ず欠落**する。

```
送信: "ver"   → ゲスト受信: "er"   → "er: command not found"
送信: "mem"   → ゲスト受信: "em"   → "em: command not found"
送信: "exec HELLO.BIN" → ゲスト受信: "xec HELLO.BIN"
```

`wait_for_boot` の `tick` コマンド (ハードコード済みバイト列) は正常に動作する。

---

## 根本原因

### `kbd_trygetchar()` によるシリアルバイトの横取り

`rshell_active = 1` の状態では、`kbd_trygetchar()` がシリアル受信バッファも自動的に読み取る設計になっている。

**kbd.c (222-227行目)**:
```c
int kbd_trygetchar(void)
{
    /* rshellモード: シリアル入力もチェック */
    if (rshell_active) {
        int sch;
        sch = serial_trygetchar();
        if (sch >= 0) return sch;  /* ← シリアルの1バイトを消費 */
    }
    ...
}
```

### 問題の発生メカニズム

`rshell.c` の `cmd_rshell()` メインループで以下の処理が行われる:

```
rshellメインループ:
  ┌─► (1) kbd_trygetchar() ← ESCキー検出 + シリアルも読む ★ここで1バイト消費
  │   (2) serial_trygetchar() ← コマンドの最初の1バイトを待つ
  │   (3) 残りのバイトを serial_trygetchar() で読み取り
  │   (4) コマンド実行
  │   (5) EOT送信
  └───────────────────────────────────────────────────────
```

**rshell.c (71-87行目)**:
```c
for (;;) {
    kch = g_api->kbd_trygetchar();     /* (1) ESCチェック ← ここでシリアル1バイト消費! */
    if (kch == 0x1B) break;

    rpos = 0;
    rbuf[0] = '\0';

    for (;;) {
        kch = g_api->kbd_trygetchar(); /* ★ さらにESCチェック → シリアル先読み */
        if (kch == 0x1B) goto rshell_exit;
        ch = g_api->serial_trygetchar(); /* (2) コマンド最初の1バイトを取得 */
        if (ch >= 0) break;              /*     → すでに(1)で消費済み → ここでは2バイト目が来る */
        ...
    }
```

### 時系列

```
時刻   WSL (rcmd.py)                     OS32 (rshell)
─────────────────────────────────────────────────────────────
T+0    パイプ接続                        ループ先頭で待機中
T+1    "v" をパイプに書き込み            →
T+2                                      kbd_trygetchar() 実行
                                           → serial_trygetchar() が "v" を取得
                                           → ESCではないので破棄 (return 'v')
                                           → メインループ外側に戻り、kch='v'判定
                                           → ESCではないのでコマンド読み取りフェーズへ
T+3    "e" をパイプに書き込み            →
T+4                                      内側ループの serial_trygetchar() が "e" を取得
                                           → rbuf[0] = 'e' (これが「最初の文字」として扱われる)
T+5    "r\n" をパイプに書き込み          →
T+6                                      残り "r\n" を読み取り → rbuf = "er"
T+7                                      "er" をコマンドとして実行 → "not found"
```

**先頭の `v` は `kbd_trygetchar()` の戻り値として消費されるが、rshellのメインループでは `kch == 0x1B` の判定にしか使われず、`v` はコマンドバッファに格納されない。**

---

## 影響範囲

- `rcmd.py` からの全コマンド送信に影響
- 手動キーボード入力には影響なし (kbd_trygetcharのキーボード側パスを通るため)
- `wait_for_boot` の `tick` は正常動作 (EOT応答のみで、先頭文字の欠落はゲスト側で "ick" として処理されるが、EOTが返るので問題なし)

---

## 修正案

### 案A: rshellメインループの修正 (推奨)

外側ループの `kbd_trygetchar()` で取得した文字がシリアル由来の場合、それをコマンドバッファの先頭バイトとして使用する。

```c
for (;;) {
    kch = g_api->kbd_trygetchar();
    if (kch == 0x1B) break;

    rpos = 0;
    rbuf[0] = '\0';

    /* ★修正: 外側ループで取得した文字がASCII印字可能文字なら
     *        コマンドの先頭バイトとして採用 */
    if (kch >= 0x20 && kch < 0x7F) {
        rbuf[rpos++] = (char)kch;
        /* 残りのバイトを読み続ける */
        goto read_rest;
    }

    for (;;) {
        kch = g_api->kbd_trygetchar();
        if (kch == 0x1B) goto rshell_exit;
        ch = g_api->serial_trygetchar();
        if (ch >= 0) break;
        ...
    }

read_rest:
    /* ch (またはkch由来) から残りを読み取り */
    ...
```

### 案B: kbd_trygetchar() からシリアル読み取りを分離

`kbd_trygetchar()` がシリアルを読むのは `kbd_getchar()` (ブロッキング版) との統一性のためだが、rshellでは `serial_trygetchar()` を直接呼んでいるため二重読み取りになる。rshell内では `kbd_trygetchar()` をキーボード専用にする方法もある。

ただしこれは `rshell_active` フラグの設計変更を伴うため、案Aの方がスコープが小さい。

### 案C: rcmd.py側でダミーバイト送信 (ワークアラウンド)

コマンドの先頭에 NUL (0x00) やスペースを1つ挿入して送信する。OS32側のコマンドパーサが先頭空白をスキップするため動作する可能性があるが、根本解決ではない。

---

## 再現手順

```bash
# NP21/W起動後、OS32シェルがrshellモードに入った状態で:
python3 tools/rcmd.py "ver"
# 期待: OS32 version情報
# 実際: "er: command not found"
```

---

## 関連ファイル

| ファイル | 役割 |
|----------|------|
| `drivers/kbd.c` L218-237 | `kbd_trygetchar()` — シリアル横取りの発生箇所 |
| `programs/shell/rshell.c` L55-125 | `cmd_rshell()` — メインループ |
| `tools/rcmd.py` L118-186 | `send_and_receive()` — コマンド送信側 |
| `drivers/serial.c` L152-164 | `serial_trygetchar()` — リングバッファからの読み取り |

---

## 修正結果 (2026-04-02)

**採用案**: 案A改良版 — `rshell.c` メインループ修正

### 修正内容

`cmd_rshell()` の外側ループおよび内側ループで `kbd_trygetchar()` が返した文字が
ESCでなく有効なASCII印字可能文字 (`0x20 <= ch < 0x7F`) の場合、コマンドバッファの
先頭バイトとして採用し、`read_rest` ラベルへジャンプして残りの文字読み取りに進む。

案Bの `kbd_trygetchar()` からシリアル読み取りを分離する方法は、VZ Editorなど
他の外部プログラムが `rshell_active` 時に `kbd_trygetchar()` 経由でシリアル入力を
受ける機能を壊すため不採用。

### 検証結果

```
送信: "ver"           → ゲスト受信: "ver"           → ✅ バージョン表示
送信: "mem"           → ゲスト受信: "mem"           → ✅ メモリ情報表示
送信: "uptime"        → ゲスト受信: "uptime"        → ✅ 稼働時間表示
送信: "mount fd0 fat12" → ゲスト受信: "mount fd0 fat12" → ✅ マウント成功
送信: "ls"            → ゲスト受信: "ls"            → ✅ ファイル一覧表示
送信: "exec HELLO.BIN" → ゲスト受信: "exec HELLO.BIN" → ✅ 外部プログラム実行
```
