#ifndef HOTDEPLOY_H
#define HOTDEPLOY_H

#include "types.h"

/* ホットデプロイ・エージェント (kernel/hotdeploy.c)
 * 設計: docs/tasks/hotdeploy/DESIGN.md */

/* ブート時に制御ブロックを初期化する */
void hotdeploy_init(void);

/* 要求があればファイル化する。割り込み文脈からは呼ばないこと。 */
void hotdeploy_poll(void);

#endif /* HOTDEPLOY_H */
