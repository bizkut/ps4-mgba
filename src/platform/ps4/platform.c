/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "platform.h"

#include <stdio.h>

void mPS4HardwareTrace(const char* message, bool reset) {
	(void) message;
	(void) reset;
}

struct mPS4Rect mPS4OutputRect(uint32_t sourceWidth, uint32_t sourceHeight,
	uint32_t outputWidth, uint32_t outputHeight, enum mPS4ScreenMode mode) {
	struct mPS4Rect rect = { 0, 0, outputWidth, outputHeight };
	if (!sourceWidth || !sourceHeight || !outputWidth || !outputHeight || mode == M_PS4_SCREEN_STRETCH) {
		return rect;
	}

	uint64_t width = outputWidth;
	uint64_t height = outputHeight;
	if (mode == M_PS4_SCREEN_PIXEL_PERFECT) {
		uint32_t scaleX = outputWidth / sourceWidth;
		uint32_t scaleY = outputHeight / sourceHeight;
		uint32_t scale = scaleX < scaleY ? scaleX : scaleY;
		if (!scale) {
			scale = 1;
		}
		width = (uint64_t) sourceWidth * scale;
		height = (uint64_t) sourceHeight * scale;
	} else if ((uint64_t) outputWidth * sourceHeight <= (uint64_t) outputHeight * sourceWidth) {
		height = (uint64_t) outputWidth * sourceHeight / sourceWidth;
	} else {
		width = (uint64_t) outputHeight * sourceWidth / sourceHeight;
	}

	if (width > outputWidth) {
		width = outputWidth;
	}
	if (height > outputHeight) {
		height = outputHeight;
	}
	rect.width = (uint32_t) width;
	rect.height = (uint32_t) height;
	rect.x = (outputWidth - rect.width) / 2;
	rect.y = (outputHeight - rect.height) / 2;
	return rect;
}

enum mPS4FilterMode mPS4FilterFromConfig(int configuredValue) {
	return configuredValue == M_PS4_FILTER_BILINEAR ? M_PS4_FILTER_BILINEAR : M_PS4_FILTER_NEAREST;
}

uint16_t mPS4MapGamePad(uint32_t buttons) {
	uint16_t keys = 0;
	if (buttons & M_PS4_PAD_CROSS) keys |= M_PS4_KEY_A;
	if (buttons & M_PS4_PAD_CIRCLE) keys |= M_PS4_KEY_B;
	if (buttons & M_PS4_PAD_TOUCH_PAD) keys |= M_PS4_KEY_SELECT;
	if (buttons & M_PS4_PAD_OPTIONS) keys |= M_PS4_KEY_START;
	if (buttons & M_PS4_PAD_RIGHT) keys |= M_PS4_KEY_RIGHT;
	if (buttons & M_PS4_PAD_LEFT) keys |= M_PS4_KEY_LEFT;
	if (buttons & M_PS4_PAD_UP) keys |= M_PS4_KEY_UP;
	if (buttons & M_PS4_PAD_DOWN) keys |= M_PS4_KEY_DOWN;
	if (buttons & M_PS4_PAD_R1) keys |= M_PS4_KEY_R;
	if (buttons & M_PS4_PAD_L1) keys |= M_PS4_KEY_L;
	return keys;
}

bool mPS4WritablePath(char* out, size_t outSize, const char* leaf) {
	if (!out || !outSize || !leaf || leaf[0] == '/' || leaf[0] == '\0') {
		return false;
	}
	int written = snprintf(out, outSize, "/data/mgba/%s", leaf);
	return written >= 0 && (size_t) written < outSize;
}

enum mPS4LifecycleAction mPS4LifecycleActionFromEvent(int32_t eventType) {
	return eventType == M_PS4_SYSTEM_EVENT_ON_RESUME ?
		M_PS4_LIFECYCLE_RESUME : M_PS4_LIFECYCLE_NONE;
}
