# _archive

アーカイブされたコンポーネント。
Git 履歴に残るため削除ではなく移動で対応。

## fdkernel/nec98/

FreeDOS(98) カスタムカーネル。
Phase 2 デバッグ中にハングで詰まり、
v86 統合リファクタリング (2026-05) で方針撤回に伴いアーカイブ。

## vdosquit.asm

DOS INT 0x21 (AH=0x4C) を使用する V86 脱出用COMプログラム。
DOS モード廃止に伴いアーカイブ (Phase D)。
ネイティブモードでは Ctrl+GRPH+DEL で脱出可能なため代替不要。
