---
name: os32-emu-config
description: OS32 のバックエンド検証に必要な NP21/W ini の限定変更。オフライン純粋変換と、明示対象に束縛したライブ停止・保存・再起動・復元を使い分ける。
---

[tools/np21w_ini_live.py](../../../tools/np21w_ini_live.py) がライブ制御、
[tools/np21w_ini.py](../../../tools/np21w_ini.py) が既存の純粋変換・オフライン準備。
ライブ制御は注入した偽 Windows executor でホスト試験済み。
**実機動作の合格を意味しない**。試験と未実施事項は
[TDD 記録](../../../tools/tests/np21w_ini_live_tdd.md) を参照。

## 範囲と根拠

[TASK_H3_cirrus.md §0](../../../docs/tasks/gui/TASK_H3_cirrus.md) と
[GUI TASKS.md の 2026-09-06 WAB OFF 記録](../../../docs/tasks/gui/TASKS.md) に従い、
`[NekoProject21]` の `USEGD5430=true/false`、`GD5430TYPE=91` のみ扱う。
91 は十進 Xe10 固定。欠落・重複・未知の既存値は追記や推測をせず拒否する。
無関係なバイト・文字コード・コメント・空白・混在改行・BOM は既存 `transform()`
で保持する。表示は変更フィールドだけ。原文や全設定を出力しない。

ホスト限定の依頼では合成データと偽 executor のみを使う。
実 ini 取得、エミュレータ照会・接続・停止・起動、配備を行わない。
`.env`、資格情報、`docs/hw/` は読まず、環境変数をロードするヘルパーを import しない [D3]。
既存 `os32-cycle`、HTTP status、tasklist の文字列一致で検証を迂回しない。
`stopped=True`、pause、breakpoint、HTTP 不在、操作者の停止宣言は終了証明にならない。

## 実機で使うための前提

- PM/ローカル操作者が非機密の起動情報と対応ソースを照合して exe と ini を選ぶ。
  WSL Python 3 と Windows PowerShell 5.1/.NET Framework、CIM のプロセス情報参照、
  対象 PID の終了、ローカル NTFS の読書き・ACL 設定、Global mutex の権限が必要。
- 対応ソースの `src/win9x/np2arg.cpp` (`Np2Arg::Parse`)、`ini.cpp`
  (`initgetfile`)、`common/milstr.c` (`milstr_getarg`) を根拠に、現在サポートする
  コマンド行は **`"C:\...\np21x64w.exe" "C:\...\selected.ini"` の 2 トークンだけ**。
  両方が引用されたドライブ絶対パスであること。相対パス、暗黙設定、`-i`/`/i`、
  追加引数、異なる引用形、UNC、デバイスパスは未対応として拒否する。
  別名 exe や設定探索を推測しない。同名/別 checkout を含め、NP2/NP21 系プロセスが
  複数あれば拒否する。選んだバイナリがこのソースに対応することは PM が確認する。
- 操作者はプレビューから再起動確認まで利用を専有し、他の起動・設定編集を防ぐ。
  `--exclusive-operator` はその前提の表明であり、終了証明ではない。
  Global named mutex は本スクリプト同士を排他する。任意の外部プロセスの起動や
  最終検査直後の書き込みまで絶対に防ぐ保証はない。書き込み前後にも再照会・再比較する。
- **停止は対象 PID の強制終了 (`Process.Kill`)**。未保存のゲスト状態を失うため、
  操作者がゲスト作業を保存し、この停止を含む変更を承認する。
  [D2](../../../docs/CONSTRAINTS.md) に従い具体的な対象と差分を提示する。
  当該操作の SCRIPT AND SKILL 経由での承認が既にあれば再質問しない。

## 操作者向け CLI

以下のパスは例。PM が選んだ実際の絶対パスに置き換える。
**既定は実対象の読取プレビュー**なので、ホスト限定作業中には実行しない。

```bash
python3 tools/np21w_ini_live.py cirrus-on --exe 'C:\NP21\np21x64w.exe' --ini 'C:\NP21\selected.ini'
# 差分確認と当該変更の承認後のみ。停止→保存→起動まで行う。
python3 tools/np21w_ini_live.py cirrus-on --exe 'C:\NP21\np21x64w.exe' --ini 'C:\NP21\selected.ini' --live-apply --exclusive-operator
# OFF は cirrus-off。GD5430TYPE=91 を維持する。
# RECEIPT_ID は適用結果の32文字ID。任意のファイルパスではない。
python3 tools/np21w_ini_live.py restore --exe 'C:\NP21\np21x64w.exe' --ini 'C:\NP21\selected.ini' --receipt RECEIPT_ID
python3 tools/np21w_ini_live.py restore --exe 'C:\NP21\np21x64w.exe' --ini 'C:\NP21\selected.ini' --receipt RECEIPT_ID --live-apply --exclusive-operator
```

変更なしは停止・保存をしない。正常終了 0、拒否/失敗 2。
初回適用は明示 ini で起動中の対象が必要。復元は、記録済みの起動情報と
成功した不在照会があれば、再起動失敗後の停止状態からも可能。

ライブ処理の順序:

1. 排他ロックを取得。CIM の成功結果から PID・exe・コマンド行・生成時刻を照合。
   成功した空配列と照会失敗/権限不足/欠落フィールドを区別する。
2. 全バイトと Windows ファイル ID・作成/更新時刻・サイズを取得し、純粋変換で候補と差分を作る。
3. PID/生成時刻を再照合し、プロセスハンドルを確保してその対象を終了。
   `WaitForExit(10000)` と成功した CIM の不在照会で終了を確認。
4. 停止後スナップショットを比較。変化していれば拒否。
   ini に隣接する固有の `*.np21w-live-ID/original.bin` に原本を保存し、
   `Flush(true)` と読み戻しを完了してから置換へ進む。バックアップは所有者専用 ACL。
5. 同じディレクトリの一時ファイルを作成・flush・検証。プロセス不在と元の内容/識別情報を
   直前に再確認し、`File.Replace` で置換。全バイト読み戻し・不在再確認後、レシートを保存。
   reparse point は親要素を含め拒否し、読み込むファイルの hardlink も拒否する。
6. 再起動前にも不在・スナップショットを比較。同じ exe と明示 ini で起動し、
   起動した PID が 1 秒以内に終了していないことと、CIM による同一対象の 2 回の確認を行う。
   作業ディレクトリは exe の親。対応ソースも起動時に `file_setcd(modulefile)` を実行する。

失敗時は自動巻き戻し・追加の起動をしない。置換後の失敗にはバックアップ ID を付ける。
バックアップやレシートが未完成なら通常の復元は拒否し、原本を残して操作者が調査する。
復元は元のターゲット、バックアップ、純粋変換の再計算、適用後の全バイトと識別情報を照合。
途中で ini を保存し直した場合も拒否する。復元処理自体のバックアップは調査用であり、
`restore` レシートをさらに `restore` する操作はサポートしない。
バンドルは原文を含むため表示・共有しない。プロセス検証成功とゲストでの反映確認は別。

## モデル/FLM 向けの限定入口

自由なシェル・任意パスをモデルに渡さない。信頼された操作者側のホストコードで
`bind_model_operation()` を束縛し、返った callable **だけ**を公開する。
リクエストは `{"operation":"cirrus-on"}`、`cirrus-off`、または
`{"operation":"restore","receipt":"32文字ID"}`。余分なフィールドは拒否する。

```python
# ホストの信頼されたセットアップ。target は PM が選択した2パス。
from np21w_ini_live import WindowsExecutor, bind_model_operation
preview = bind_model_operation(WindowsExecutor(target), target)
# D2 の当該差分の承認後、承認内容そのものを束縛する。
approved = {"operation": "cirrus-on"}
apply_once = bind_model_operation(
    WindowsExecutor(target), target, authorized_apply=True, exclusive=True,
    approved_request=approved)
# モデルへは apply_once(request) のみ公開。承認は1回の試行で消費する。
```

この入口と CLI は実装済み。既存 `emu_agent` の自由シェル許可リストを広げない。
今回の変更範囲には既存エージェントへの自動登録は含まれないため、PM のホストで
上の callable を限定アクションとして接続するか、操作者が CLI を直接使う。
停止検証器を別途実装する必要はない。

## オフライン準備と反映確認

従来のオフライン手順も利用可能。入力スナップショット自体は更新しない。

```bash
python3 tools/np21w_ini.py prepare INPUT.snapshot --set USEGD5430=true --set GD5430TYPE=91
python3 tools/np21w_ini.py prepare INPUT.snapshot --set USEGD5430=true --set GD5430TYPE=91 --output OUTPUT_DIR --apply
python3 tools/np21w_ini.py restore BUNDLE
python3 tools/np21w_ini.py restore BUNDLE --apply
```

オフライン `--apply` は新規専用バンドルを作るだけ。実 ini へコピーしてライブ検証を迂回しない。
実機で承認・実行した後は `ver`、`hal_test` の backend、画面、必要な `wab_relay` を観測する。
本スクリプトはゲスト試験・配備をしない。HostDrv だけでは反映検証にならない [V1]。
NHD/ブート配備は別の明示範囲に従い停止中のみ [D1]。未実施はそのまま報告する [V4]。
