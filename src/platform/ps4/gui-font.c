/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-font.h"
#include "renderer.h"

#include <mgba-util/gui/font.h>
#include <mgba-util/gui/font-metrics.h>
#include <mgba-util/image/png-io.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <stdlib.h>

enum {
	GLYPH_HEIGHT = 24,
	CELL_WIDTH = 32,
	CELL_HEIGHT = 32,
	ATLAS_WIDTH = 512,
	ATLAS_HEIGHT = 512,
};

struct GUIFont {
	struct mPS4Renderer* renderer;
	unsigned texture;
};

static struct mPS4Renderer* s_renderer;

void mPS4GUIFontSetRenderer(struct mPS4Renderer* renderer) {
	s_renderer = renderer;
}

static bool loadAtlas(uint32_t** rgba) {
	struct VFile* vf = VFileOpen("/app0/assets/font-new.png", O_RDONLY);
	if (!vf) return false;
	png_structp png = PNGReadOpen(vf, 0);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	png_infop end = png ? png_create_info_struct(png) : NULL;
	bool success = png && info && end && PNGReadHeader(png, info) &&
		png_get_image_width(png, info) == ATLAS_WIDTH &&
		png_get_image_height(png, info) == ATLAS_HEIGHT;
	uint8_t* alpha = NULL;
	uint32_t* pixels = NULL;
	if (success) {
		alpha = malloc(ATLAS_WIDTH * ATLAS_HEIGHT);
		pixels = malloc(ATLAS_WIDTH * ATLAS_HEIGHT * sizeof(*pixels));
		success = alpha && pixels && PNGReadPixels8(png, info, alpha,
			ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_WIDTH) && PNGReadFooter(png, end);
	}
	if (success) {
		for (unsigned i = 0; i < ATLAS_WIDTH * ATLAS_HEIGHT; ++i) {
			pixels[i] = 0x00FFFFFFU | (uint32_t) alpha[i] << 24;
		}
		*rgba = pixels;
	} else {
		free(pixels);
	}
	free(alpha);
	PNGReadClose(png, info, end);
	vf->close(vf);
	return success;
}

struct GUIFont* GUIFontCreate(void) {
	if (!s_renderer) return NULL;
	struct GUIFont* font = calloc(1, sizeof(*font));
	uint32_t* pixels = NULL;
	if (!font || !loadAtlas(&pixels)) {
		free(font);
		return NULL;
	}
	font->renderer = s_renderer;
	bool success = mPS4RendererCreateTexture(font->renderer, ATLAS_WIDTH,
		ATLAS_HEIGHT, pixels, ATLAS_WIDTH, &font->texture);
	free(pixels);
	if (!success) {
		free(font);
		return NULL;
	}
	return font;
}

void GUIFontDestroy(struct GUIFont* font) {
	free(font);
}

unsigned GUIFontHeight(const struct GUIFont* font) {
	UNUSED(font);
	return GLYPH_HEIGHT;
}

unsigned GUIFontGlyphWidth(const struct GUIFont* font, uint32_t glyph) {
	UNUSED(font);
	if (glyph > 0x7F) glyph = '?';
	return defaultFontMetrics[glyph].width * 2;
}

void GUIFontIconMetrics(const struct GUIFont* font, enum GUIIcon icon,
	unsigned* width, unsigned* height) {
	UNUSED(font);
	if (icon >= GUI_ICON_MAX) {
		if (width) *width = 0;
		if (height) *height = 0;
		return;
	}
	if (width) *width = defaultIconMetrics[icon].width * 2;
	if (height) *height = defaultIconMetrics[icon].height * 2;
}

static struct mPS4Color colorFromU32(uint32_t color) {
	return (struct mPS4Color) {
		(color & 0xFF) / 255.0f,
		((color >> 8) & 0xFF) / 255.0f,
		((color >> 16) & 0xFF) / 255.0f,
		((color >> 24) & 0xFF) / 255.0f,
	};
}

static void drawAtlas(struct GUIFont* font, int x, int y, int width, int height,
	float left, float top, float right, float bottom, uint32_t color) {
	if (width <= 0 || height <= 0) return;
	mPS4RendererAddQuad(font->renderer, font->texture,
		(struct mPS4Rect) { (uint32_t) x, (uint32_t) y, (uint32_t) width, (uint32_t) height },
		(struct mPS4UvRect) { left, top, right, bottom }, colorFromU32(color),
		M_PS4_FILTER_BILINEAR);
}

void GUIFontDrawGlyph(struct GUIFont* font, int x, int y, uint32_t color, uint32_t glyph) {
	if (glyph > 0x7F) glyph = '?';
	struct GUIFontGlyphMetric metric = defaultFontMetrics[glyph];
	int sourceX = (glyph & 15) * CELL_WIDTH + metric.padding.left * 2;
	int sourceY = (glyph >> 4) * CELL_HEIGHT + metric.padding.top * 2;
	int width = CELL_WIDTH - (metric.padding.left + metric.padding.right) * 2;
	int height = CELL_HEIGHT - (metric.padding.top + metric.padding.bottom) * 2;
	drawAtlas(font, x, y - GLYPH_HEIGHT + metric.padding.top * 2, width, height,
		(float) sourceX / ATLAS_WIDTH, (float) sourceY / ATLAS_HEIGHT,
		(float) (sourceX + width) / ATLAS_WIDTH,
		(float) (sourceY + height) / ATLAS_HEIGHT, color);
}

static void alignIcon(int* x, int* y, int width, int height, enum GUIAlignment align) {
	if ((align & GUI_ALIGN_HCENTER) == GUI_ALIGN_HCENTER) *x -= width / 2;
	else if ((align & GUI_ALIGN_RIGHT) == GUI_ALIGN_RIGHT) *x -= width;
	if ((align & GUI_ALIGN_VCENTER) == GUI_ALIGN_VCENTER) *y -= height / 2;
	else if ((align & GUI_ALIGN_BOTTOM) == GUI_ALIGN_BOTTOM) *y -= height;
}

void GUIFontDrawIcon(struct GUIFont* font, int x, int y, enum GUIAlignment align,
	enum GUIOrientation orient, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) return;
	struct GUIIconMetric metric = defaultIconMetrics[icon];
	int width = metric.width * 2;
	int height = metric.height * 2;
	alignIcon(&x, &y, width, height, align);
	float left = (float) (metric.x * 2) / ATLAS_WIDTH;
	float top = (float) (metric.y * 2 + 256) / ATLAS_HEIGHT;
	float right = left + (float) width / ATLAS_WIDTH;
	float bottom = top + (float) height / ATLAS_HEIGHT;
	if (orient == GUI_ORIENT_HMIRROR || orient == GUI_ORIENT_180) {
		float swap = left; left = right; right = swap;
	}
	if (orient == GUI_ORIENT_VMIRROR || orient == GUI_ORIENT_180) {
		float swap = top; top = bottom; bottom = swap;
	}
	drawAtlas(font, x, y, width, height, left, top, right, bottom, color);
}

void GUIFontDrawIconSize(struct GUIFont* font, int x, int y, int width, int height,
	uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) return;
	struct GUIIconMetric metric = defaultIconMetrics[icon];
	if (!width) width = metric.width * 2;
	if (!height) height = metric.height * 2;
	float left = (float) (metric.x * 2) / ATLAS_WIDTH;
	float top = (float) (metric.y * 2 + 256) / ATLAS_HEIGHT;
	drawAtlas(font, x, y, width, height, left, top,
		left + (float) (metric.width * 2) / ATLAS_WIDTH,
		top + (float) (metric.height * 2) / ATLAS_HEIGHT, color);
}
