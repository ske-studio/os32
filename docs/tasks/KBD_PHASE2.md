# Phase 2: キーボードLED制御 (0x9Dh) — FEP連携

## 概要
UNDOCUMENTED io_kb.md の下りコマンド `0x9Dh` を使用し、キーボードのCAPS/カナ/NUM LEDをソフトウェアから直接制御する。FEP（日本語入力）の入力モードをLEDで物理的に通知するために必要。

## 背景
- OS32はプロテクトモードで動作しBIOSを経由しないため、LED状態がBIOSと不一致になる
- FEP実装時、日本語入力ON/OFFを「かなLED」で示すのはPC-98の標準的UX
- CAPS Lockの状態もOS32側で管理する必要がある

## 出典
- UNDOCUMENTED 9801/9821 Vol.2 io_kb.md — I/O 0041h WRITE コマンド `9Dh`
- 対象: 新キーボード搭載機

## 9Dhコマンド仕様
```
LED読み出し: 9Dh → キーボードから返答データを受信
LED設定:     9Dh 7xh
  x の各ビット:
    bit 2: NUM Lock LED
    bit 1: CAPS Lock LED
    bit 0: カナ Lock LED
  (0=消灯, 1=点灯)
```

## タスクリスト
- [ ] Phase 1 の `kbd_send_cmd()` を前提とする
- [ ] `kbd_set_led(u8 led_mask)` 関数を実装
  - `kbd_send_cmd(0x9D)` → `kbd_send_cmd(0x70 | (led_mask & 0x07))` の2バイト送信
- [ ] `kbd_get_led()` 関数を実装（オプション）
  - `0x9Dh` 送信後、キーボードからの返答データを受信
  - 受信にはIRQ1割り込み経由のバッファからの読み出しが必要
- [ ] FEPのモード切替時に `kbd_set_led()` を呼び出すフックを設計
  - かなLED: FEP ON/OFF
  - CAPS LED: 英大文字モード
- [ ] KAPI に `kbd_set_led` を公開
- [ ] OS32起動時にLED初期状態を設定 (全消灯)

## 依存関係
- Phase 1 (kbd_send_cmd 基盤) が先に必要
- FEPアーキテクチャ設計 (@conversation 8d7218e7) との連携

## 注意事項
- LED読み出し (`9Dh` 単体) はキーボードからの「返答」を待つ必要があるが、現在のIRQ1ハンドラはキースキャンコードの受信のみ対応。コマンド応答の判別ロジックが必要
- NP21/WのLEDエミュレーション対応状況は要確認
