# TASK AUDIO — software mixer / CD-DA / software FM

## 目的

PE32/GUI本線を待たず、OS32 native audio 基盤を独立実装できる状態にする。
将来 WinMM/DirectSound wrapper も同じ native mixer に接続可能とする。

## 必読

- ../PLAN.md V3-23 / Q24
- ../MIGRATION_AUDIT.md M31
- 既存 sound / PCM / 9801-26/86 関連コード

## 固定方針

- mixer はスクラッチ実装。
- source PCM: 16bit。
- mix accumulator: 32bit signed integer。
- gain/pan: Q1.15。
- final: 16bit hard saturation。
- resampling は mixer 外、source側責務。
- unity/mute fast path を用意。
- ring buffer と underrun/overrun counter を持つ。
- CD-DA digital を既定、analog は fallback。

## Subtask A1: mixer core

入力:
- 複数 PCM source
- gain/pan
- output buffer

実装:
- signed 32bit accumulation
- master gain
- saturation
- mute/unity fast path
- source lifecycle
- buffer accounting

受入:
- 1 source bit-exact unity
- 0dB相当2source加算でclipping時にwrapしない
- mute
- pan L/R
- 最大source数でoverflowしない
- underrun/overrun計数

## Subtask A2: CD-DA digital source

実装:
- ATAPI READ CD 等から 44.1kHz/16bit/stereo PCM を取得
- mixer source に供給
- `CD_AUDIO=digital`
- `CD_AUDIO=analog` fallback を保持

受入:
- CD-DA単独連続再生
- CD-DA + PCM SFX
- digital失敗時の明示エラー/analog選択
- DX4/Pentium級CPU負荷計測

## Subtask A3: software FM adapter

実装:
- FM engineを直接mixer ABIへ露出しない。
- FM backend→PCM source adapter を定義。
- fmgen等を候補にする場合はライセンス/整数演算/メモリ量を別途記録。

受入:
- PCM SFX と software FM の同時再生
- mixer側にFM固有型が存在しない。

## 禁止

- mixerの都合で KAPI に fmgen 固有型を出す。
- network/host audio streaming をこの票に混ぜる。
- resamplerをmixer本体へ固定する。

## COREへの要求方法

新しいKAPI/ABIが必要な場合、必要な意味操作・型・lifetime・errorだけを記載してCOREへ渡す。
番号や公開ABIをAUDIO側で独断確定しない。

## 完了報告

- source/mixer API
- CPU負荷
- buffer size/latency
- clipping/underrun試験
- CD-DA実機結果
- FM backend候補とライセンス
