# TASK HOST — Host Services provider 基盤

## 目的

既存 Host Services を、個別KAPIを増やさず現代機能を外部 provider から借りる共通サービス基盤へ拡張する。

## 必読

- ../PLAN.md V3-19 / V3-20 / Q20 / Q21
- ../../network/HOST_SERVICES_PLAN.md
- ../MIGRATION_AUDIT.md M27 / M28

## 既存契約として守ること

- OS32 に TCP/IP/DNS/HTTP(S)/TLS/X.509 を実装しない。
- OS32 は request/result/stream のみ扱う。
- provider差 (Windows/Linux/Android/OS64/cloud/local AI) をOS32へ漏らさない。
- host未接続を成功扱いしない。

## Subtask H1: common service envelope

最低限検討:
- service
- version
- capability
- request
- stream
- status
- error
- provider identification

要求:
- 未知service/versionを安全に拒否。
- capability discovery。
- 長い本文はstream。
- provider固有errorを安定したOS32 error/statusへ写像。

## Subtask H2: provider mock

host側だけで以下の mock provider を作り、OS32側契約を検証する。
- document transform
- image transform
- storage list/read
- language/translation mock
- AI structured result mock

実サービス接続は必須ではない。

## Subtask H3: transport independence

既存 raw Ethernet/Host Agent と service contract を分離する。
将来別transportを使ってもservice APIを変えない。

## 非対象

- OpenSSL導入
- OS32側HTTP stack
- cloud SDKのOS32移植
- providerごとのKAPI追加
- WinSock互換そのもの

## 受入

- 2種類以上の provider mock が同一OS32 contractで交換可能。
- capability不足/host offline/version mismatch が区別される。
- 既存 GET/TIME/PRINT/CLIP の意味を壊さない。
- KAPI増殖なしで新serviceを追加できる。

## COREへの要求

共通ABIに変更が必要なら、provider名やクラウド固有概念ではなく
service/version/request/stream/status/error の意味で要求する。

## 完了報告

- wire/service schema
- backward compatibility
- provider mock結果
- error map
- 既存Host Servicesとの差分
