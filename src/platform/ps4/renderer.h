/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_PS4_RENDERER_H
#define M_PS4_RENDERER_H

#include "platform.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	M_PS4_LOGICAL_WIDTH = 1280,
	M_PS4_LOGICAL_HEIGHT = 720,
	M_PS4_FRAME_TEXTURE_WIDTH = 256,
	M_PS4_FRAME_TEXTURE_HEIGHT = 256,
	M_PS4_FRAME_TEXTURE_COUNT = 2,
	M_PS4_SOLID_TEXTURE = 2,
	M_PS4_MAX_TEXTURES = 4,
};

struct mPS4Renderer;

struct mPS4Color {
	float r;
	float g;
	float b;
	float a;
};

struct mPS4UvRect {
	float left;
	float top;
	float right;
	float bottom;
};

bool mPS4RendererCreate(struct mPS4Renderer** renderer);
void mPS4RendererDestroy(struct mPS4Renderer* renderer);
bool mPS4RendererRunReferenceTriangle(struct mPS4Renderer* renderer);
bool mPS4RendererRunReferenceBlit(struct mPS4Renderer* renderer);

uint32_t* mPS4RendererFramePixels(struct mPS4Renderer* renderer,
	unsigned textureIndex, uint32_t* stridePixels);
bool mPS4RendererCreateTexture(struct mPS4Renderer* renderer, uint32_t width,
	uint32_t height, const uint32_t* pixels, uint32_t stridePixels,
	unsigned* textureIndex);

bool mPS4RendererBegin(struct mPS4Renderer* renderer);
void mPS4RendererCancel(struct mPS4Renderer* renderer);
bool mPS4RendererAddQuad(struct mPS4Renderer* renderer, unsigned textureIndex,
	struct mPS4Rect destination, struct mPS4UvRect uv, struct mPS4Color color,
	enum mPS4FilterMode filter);
bool mPS4RendererEnd(struct mPS4Renderer* renderer);

bool mPS4RendererPresentFrame(struct mPS4Renderer* renderer, unsigned textureIndex,
	uint32_t sourceWidth, uint32_t sourceHeight, enum mPS4ScreenMode screenMode,
	enum mPS4FilterMode filter);

#endif
