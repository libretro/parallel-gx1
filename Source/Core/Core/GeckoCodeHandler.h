/* Copyright 2010-2017 Nuke, brkirch, Y.S, Kenobi, gamemasterplc
 * SPDX-License-Identifier: MIT */

/* The Gecko code handler, built into the core: the PowerPC code the game runs
 * each frame to apply the active Gecko codes. */

#ifndef GECKO_CODE_HANDLER_H
#define GECKO_CODE_HANDLER_H

#define GECKO_CODE_HANDLER_SIZE 2880

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char gecko_code_handler[GECKO_CODE_HANDLER_SIZE];

#ifdef __cplusplus
}
#endif

#endif
