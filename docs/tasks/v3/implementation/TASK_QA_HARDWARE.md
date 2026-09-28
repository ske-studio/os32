# TASK QA — 実機・エミュレータ・性能受入

## 目的

v3各レーンの結果を、古い結果やエミュレータだけで合格扱いしないための共通受入基盤を担当する。

## 必読

- ../PLAN.md P3 / Q02 / Q10
- ../DEBUG_AND_MODULES.md
- ../MIGRATION_AUDIT.md
- ../../TESTS.md

## Subtask Q1: result channel

必須:
- build/boot identity
- test id
- start/end
- pass/fail/skip
- timeout
- panic/fault
- environment
を機械可読に記録。

未実行/古い結果/途中切れをpassにしない。

## Subtask Q2: emulator workflow

- clean build
- deploy identity
- boot
- guest batch
- result collection
- failure artifact
を再現可能にする。

## Subtask Q3: physical hardware workflow

最低:
- target machine identity
- deployed build identity
- boot media
- serial/console/screenshot等の証拠
- known-good rollback
- fault記録

Ra266等の物理機では、エミュレータとの差を明示。

## Subtask Q4: PC-98 video regression

既知の観測:
- Ra266起動時 31kHz 720x350 扱い
- OS32 GUI既定video modeで 31kHz 640x480 に遷移し表示崩れ

これを再現可能な試験項目にする。
v2.1修正とv3回帰を分離して記録。

## Subtask Q5: performance budget

測定対象:
- DX4/16MB CUI
- mixer: CD-DA単独 / CD-DA+SFX
- PE32 loader
- GUI blit/convert/text/composite
- Host Services stream

CPU、RAM peak、latency、underrun等を記録。

## 受入

- 同じbuildを emulator/real hardware で識別できる。
- timeout/panicを自動成功にしない。
- known-goodへ戻せる。
- 最低性能目標に対する定量データがある。

## 完了報告

- test runner/format
- tested hardware
- build ids
- performance table
- unreproduced issues
