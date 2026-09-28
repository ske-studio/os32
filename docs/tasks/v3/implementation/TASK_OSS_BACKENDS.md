# TASK OSS — native abstraction / backend 分離

## 目的

FreeType・画像codec・圧縮・音声codec等を、OSS固有ABIを公開せず交換可能backendとして利用できる境界を作る。

## 必読

- ../PLAN.md V3-19 / Q20
- ../PORTABILITY_PRINCIPLES.md
- ../MIGRATION_AUDIT.md M27

## 原則

- OS32 public API/KAPI は OSS ライブラリの型を露出しない。
- backendは差し替え可能。
- capability queryを持つ。
- host無しでも価値があるローカル処理を優先。
- RAM/CPU負荷が対象機の能力を越える場合は未対応を明示。

## Subtask O1: font abstraction

FreeType等を候補として:
- face open/close
- glyph metrics
- rasterize
- encoding/glyph mapping
- cache policy
をOS32 native意味へ整理。

非対象:
- FT_Face等を公開ABIにすること。

## Subtask O2: image/codec abstraction

候補:
- PNG/JPEG等の既存軽量形式
- 将来backend追加

最低操作:
- probe
- decode metadata
- decode to native bitmap
- capability/unsupported

現代codec(WebP/AVIF/HEIF等)は、ローカル負荷が不適切ならHost Servicesへ委譲可能。

## Subtask O3: license/performance ledger

各候補ごとに:
- license
- linking/distribution条件
- code size
- RAM peak
- CPU class
- dependency
を記録。

## 受入

- mock backendと実backendを同じnative APIで交換できる。
- OSS固有型がKAPI/standard app APIにない。
- backend無し/機能不足が明示errorになる。
- DX4/Pentium級で採用可否判断に必要な計測がある。

## 完了報告

- native abstraction
- backend候補
- license表
- RAM/CPU計測
- Host Servicesへ逃がす対象
