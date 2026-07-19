/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_PS4_PLATFORM_H
#define M_PS4_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mPS4ScreenMode {
	M_PS4_SCREEN_PIXEL_PERFECT,
	M_PS4_SCREEN_ASPECT_FIT,
	M_PS4_SCREEN_STRETCH,
};

enum mPS4FilterMode {
	M_PS4_FILTER_NEAREST,
	M_PS4_FILTER_BILINEAR,
};

enum mPS4LifecycleAction {
	M_PS4_LIFECYCLE_NONE,
	M_PS4_LIFECYCLE_RESUME,
};

enum {
	M_PS4_SYSTEM_EVENT_ON_RESUME = 0x10000000,
};

struct mPS4Rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

enum mPS4PadButton {
	M_PS4_PAD_UP = 1U << 0,
	M_PS4_PAD_DOWN = 1U << 1,
	M_PS4_PAD_LEFT = 1U << 2,
	M_PS4_PAD_RIGHT = 1U << 3,
	M_PS4_PAD_CROSS = 1U << 4,
	M_PS4_PAD_CIRCLE = 1U << 5,
	M_PS4_PAD_L1 = 1U << 6,
	M_PS4_PAD_R1 = 1U << 7,
	M_PS4_PAD_OPTIONS = 1U << 8,
	M_PS4_PAD_TOUCH_PAD = 1U << 9,
};

enum mPS4GameKey {
	M_PS4_KEY_A = 1U << 0,
	M_PS4_KEY_B = 1U << 1,
	M_PS4_KEY_SELECT = 1U << 2,
	M_PS4_KEY_START = 1U << 3,
	M_PS4_KEY_RIGHT = 1U << 4,
	M_PS4_KEY_LEFT = 1U << 5,
	M_PS4_KEY_UP = 1U << 6,
	M_PS4_KEY_DOWN = 1U << 7,
	M_PS4_KEY_R = 1U << 8,
	M_PS4_KEY_L = 1U << 9,
};

struct mPS4Rect mPS4OutputRect(uint32_t sourceWidth, uint32_t sourceHeight,
	uint32_t outputWidth, uint32_t outputHeight, enum mPS4ScreenMode mode);
enum mPS4FilterMode mPS4FilterFromConfig(int configuredValue);
uint16_t mPS4MapGamePad(uint32_t buttons);
bool mPS4WritablePath(char* out, size_t outSize, const char* leaf);
enum mPS4LifecycleAction mPS4LifecycleActionFromEvent(int32_t eventType);
void mPS4HardwareTrace(const char* message, bool reset);

#endif
