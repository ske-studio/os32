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

## Subtask H4: deferred host event / pending request queue

目的:
- プリエンプティブ・マルチタスクや常駐アプリを導入せず、host側で発生した更新要求を対象アプリの次回起動時に配送する。
- ユーザーからは、アプリ終了中にも更新準備が進み、起動時に最新情報へ追従できるように見せる。
- これはバックグラウンド実行ではなく、host-driven deferred event として扱う。

基本フロー:

```text
Host
  -> Kernel pending queue
  -> target application starts
  -> kernel notifies "pending host event exists"
  -> application requests event metadata
  -> application queries Host Service
  -> latest result is fetched normally
```

キューに保持するもの:
- target application / application class
- service
- event kind
- generation または単調増加ID
- 必要なら最小限の flags

キューに保持しないもの:
- 天気情報、ニュース本文、画像等のサービスデータ本体
- provider固有JSONやクラウドSDK固有データ
- 大容量payload
- アプリ固有の実行状態

例:

```text
target_app = weather
service    = weather
event      = refresh_available
generation = 184
```

要求:
- kernelは「問い合わせるべきものがある」ことだけを保持し、結果本体のcacheにはならない。
- 実データ取得は対象アプリ起動後に通常の Host Services request/result/stream 契約で行う。
- 同一 target/service/event の古い通知は generation により集約できること。不要に同種イベントを積み続けない。
- host側で pending list の追加・更新・削除・集約を行えること。
- 未知target、未知service、version mismatch、host offline を安全に処理すること。
- アプリがイベントを処理しなくてもkernelや他アプリの進行を妨げないこと。
- OS32側で任意コードを自動起動しないこと。
- timer interrupt等でアプリを強制切替しないこと。
- この機構をプリエンプティブ・マルチタスク化の入口にしないこと。

受入例:
- weatherアプリ終了中にhostが複数回 refresh を要求しても、次回起動時には最新 generation の1件として通知できる。
- host未接続時はpending通知そのものと実データ取得失敗を区別できる。
- pending event を持たないアプリには追加処理が発生しない。
- 通知受領後に通常の Host Service API だけで最新情報を取得できる。
- queue内にサービス結果本体を保存しないことをテストで確認する。

設計原則:

> アプリを裏で動かすのではなく、アプリが次に動いた時に、裏で発生した世界の変化を引き渡す。

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
