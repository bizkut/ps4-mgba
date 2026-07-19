/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "renderer.h"

#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <gnm/drawcommandbuffer.h>
#include <gnm/gpuaddr/gpuaddr.h>
#include <gnm/platform.h>
#include <gnm/shaderbinary.h>
#include <gnm_helpers.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	BUFFER_COUNT = 2,
	DIRECT_MEMORY_SIZE = 192 * 1024 * 1024,
	DIRECT_MEMORY_ALIGNMENT = 64 * 1024,
	COMMAND_BUFFER_SIZE = 256 * 1024,
	MAX_QUADS = 512,
	VERTICES_PER_QUAD = 6,
};

struct mPS4Vertex {
	float position[2];
	float uv[2];
	float color[4];
};

struct mPS4Quad {
	struct mPS4Vertex vertices[VERTICES_PER_QUAD];
	unsigned texture;
	enum mPS4FilterMode filter;
};

struct mPS4PsDescriptors {
	GnmTexture texture;
	GnmSampler sampler;
};

struct mPS4BlitVertex {
	float position[2];
	float uv[2];
};

struct mPS4Renderer {
	off_t direct;
	uint8_t* base;
	size_t offset;
	int video;
	OrbisKernelEqueue flips;
	uint32_t outputWidth;
	uint32_t outputHeight;
	GnmRenderTarget targets[BUFFER_COUNT];
	GnmTexture textures[M_PS4_MAX_TEXTURES];
	uint32_t* texturePixels[M_PS4_MAX_TEXTURES];
	unsigned textureCount;
	GnmCommandBuffer command;
	volatile uint64_t* label;
	uint64_t frame;
	GnmVsShader* vertexShader;
	GnmPsShader* pixelShader;
	void* fetchShader;
	struct mPS4Vertex* vertexMemory;
	struct mPS4Quad quads[MAX_QUADS];
	unsigned quadCount;
	bool frameOpen;
};

static size_t alignUp(size_t value, size_t alignment) {
	return (value + alignment - 1) / alignment * alignment;
}

static void* allocGarlic(struct mPS4Renderer* renderer, size_t size, size_t alignment) {
	size_t offset = alignUp(renderer->offset, alignment);
	if (offset > DIRECT_MEMORY_SIZE || size > DIRECT_MEMORY_SIZE - offset) {
		return NULL;
	}
	void* result = renderer->base + offset;
	renderer->offset = offset + size;
	return result;
}

static bool readFile(const char* path, void** data, size_t* size) {
	FILE* file = fopen(path, "rb");
	if (!file || fseek(file, 0, SEEK_END)) {
		if (file) fclose(file);
		return false;
	}
	long length = ftell(file);
	if (length <= 0 || fseek(file, 0, SEEK_SET)) {
		fclose(file);
		return false;
	}
	void* bytes = malloc((size_t) length);
	if (!bytes || fread(bytes, 1, (size_t) length, file) != (size_t) length) {
		free(bytes);
		fclose(file);
		return false;
	}
	fclose(file);
	*data = bytes;
	*size = (size_t) length;
	return true;
}

static bool loadVertexShader(struct mPS4Renderer* renderer, const char* path,
	unsigned expectedInputs, unsigned expectedExports, GnmVsShader** outShader) {
	void* fileData = NULL;
	size_t fileSize = 0;
	GnmShaderMetadata metadata = {0};
	if (!readFile(path, &fileData, &fileSize) ||
		sceGnmShaderBinaryGetMetadata(fileData, fileSize, &metadata) != GNM_ERROR_OK ||
		metadata.type != GNM_SHADER_VERTEX || metadata.numinputsemantics != expectedInputs ||
		metadata.numexportsemantics != expectedExports || !metadata.stage || !metadata.shadercode) {
		free(fileData);
		return false;
	}
	GnmVsShader* shader = malloc(metadata.stagesize);
	void* code = allocGarlic(renderer, metadata.shadercodesize, GNM_ALIGNMENT_SHADER_BYTES);
	if (!shader || !code) {
		free(shader);
		free(fileData);
		return false;
	}
	memcpy(shader, metadata.stage, metadata.stagesize);
	memcpy(code, metadata.shadercode, metadata.shadercodesize);
	gnmVsRegsSetAddress(&shader->registers, code);
	free(fileData);
	*outShader = shader;
	return true;
}

static bool loadPixelShader(struct mPS4Renderer* renderer, const char* path,
	unsigned expectedInputs, GnmPsShader** outShader) {
	void* fileData = NULL;
	size_t fileSize = 0;
	GnmShaderMetadata metadata = {0};
	if (!readFile(path, &fileData, &fileSize) ||
		sceGnmShaderBinaryGetMetadata(fileData, fileSize, &metadata) != GNM_ERROR_OK ||
		metadata.type != GNM_SHADER_PIXEL || metadata.numinputsemantics != expectedInputs ||
		!metadata.stage || !metadata.shadercode) {
		free(fileData);
		return false;
	}
	GnmPsShader* shader = malloc(metadata.stagesize);
	void* code = allocGarlic(renderer, metadata.shadercodesize, GNM_ALIGNMENT_SHADER_BYTES);
	if (!shader || !code) {
		free(shader);
		free(fileData);
		return false;
	}
	memcpy(shader, metadata.stage, metadata.stagesize);
	memcpy(code, metadata.shadercode, metadata.shadercodesize);
	gnmPsRegsSetAddress(&shader->registers, code);
	shader->registers.spibaryccntl = 0;
	free(fileData);
	*outShader = shader;
	return true;
}

static bool createTarget(struct mPS4Renderer* renderer, GnmRenderTarget* target) {
	GpaSurfaceProperties properties = {0};
	if (gpaFindOptimalSurface(&properties, GPA_SURFACE_COLORDISPLAY, 32, 1, false,
		gnmGpuMode()) != GPA_ERR_OK) {
		return false;
	}
	const GnmRenderTargetCreateInfo createInfo = {
		.width = renderer->outputWidth,
		.height = renderer->outputHeight,
		.numslices = 1,
		.colorfmt = GNM_FMT_R8G8B8A8_SRGB,
		.colortilemodehint = properties.tilemode,
		.mingpumode = gnmGpuMode(),
		.numsamples = 1,
		.numfragments = 1,
	};
	if (gnmCreateRenderTarget(target, &createInfo) != GNM_ERROR_OK) {
		return false;
	}
	uint64_t size = 0;
	uint32_t alignment = 0;
	if (gnmRtCalcByteSize(&size, &alignment, target) != GNM_ERROR_OK) {
		return false;
	}
	void* memory = allocGarlic(renderer, (size_t) size, alignment);
	if (!memory) {
		return false;
	}
	memset(memory, 0, (size_t) size);
	gnmRtSetBaseAddr(target, memory);
	return true;
}

static bool createTexture(struct mPS4Renderer* renderer, unsigned index,
	uint32_t width, uint32_t height, const uint32_t* pixels, uint32_t stridePixels) {
	const GnmTextureCreateInfo createInfo = {
		.texturetype = GNM_TEXTURE_2D,
		.width = width,
		.height = height,
		.depth = 1,
		.pitch = width,
		.nummiplevels = 1,
		.numslices = 1,
		.format = GNM_FMT_R8G8B8A8_SRGB,
		.tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL,
		.mingpumode = gnmGpuMode(),
		.numfragments = 1,
	};
	if (gnmCreateTexture(&renderer->textures[index], &createInfo) != GNM_ERROR_OK) {
		return false;
	}
	renderer->texturePixels[index] = allocGarlic(renderer,
		width * height * sizeof(uint32_t),
		GNM_ALIGNMENT_SHADER_BYTES);
	if (!renderer->texturePixels[index]) {
		return false;
	}
	if (pixels) {
		for (uint32_t y = 0; y < height; ++y) {
			memcpy(renderer->texturePixels[index] + y * width,
				pixels + y * stridePixels, width * sizeof(uint32_t));
		}
	} else {
		memset(renderer->texturePixels[index], 0, width * height * sizeof(uint32_t));
	}
	gnmTexSetBaseAddress(&renderer->textures[index], renderer->texturePixels[index]);
	return true;
}

static bool createFetchShaderFor(struct mPS4Renderer* renderer, GnmVsShader* shader,
	void** outFetchShader) {
	const GnmFetchShaderCreateInfo createInfo = {
		.regs = &shader->registers,
		.vtxinputs = gnmVsShaderInputSemanticTable(shader),
		.numvtxinputs = shader->numinputsemantics,
		.inputusages = gnmVsShaderInputUsageSlotTable(shader),
		.numinputusages = shader->common.numinputusageslots,
	};
	uint32_t size = 0;
	if (gnmFetchShaderCalcSize(&size, &createInfo) != GNM_ERROR_OK) {
		return false;
	}
	void* fetchShader = allocGarlic(renderer, size, GNM_ALIGNMENT_FETCHSHADER_BYTES);
	GnmFetchShaderResults results = {0};
	if (!fetchShader || gnmCreateFetchShader(fetchShader, size,
		&createInfo, &results) != GNM_ERROR_OK) {
		return false;
	}
	gnmVsRegsSetFetchShaderModifier(&shader->registers, &results);
	*outFetchShader = fetchShader;
	return true;
}

static bool createFetchShader(struct mPS4Renderer* renderer) {
	return createFetchShaderFor(renderer, renderer->vertexShader, &renderer->fetchShader);
}

static void setupViewport(GnmCommandBuffer* command, uint32_t width, uint32_t height) {
	const GnmSetViewportInfo viewport = {
		.dmin = 0.0f,
		.dmax = 1.0f,
		.scale = { width * 0.5f, height * -0.5f, 0.5f },
		.offset = { width * 0.5f, height * 0.5f, 0.5f },
	};
	const GnmViewportTransformControl transform = {
		.scalex = 1, .offsetx = 1, .scaley = 1, .offsety = 1,
		.scalez = 1, .offsetz = 1, .invertw = 1,
	};
	gnmDrawCmdSetViewport(command, 0, &viewport);
	gnmDrawCmdSetViewportTransformControl(command, &transform);
	gnmDrawCmdSetScreenScissor(command, 0, 0, width, height);
	gnmDrawCmdSetHwScreenOffset(command, 0, 0);
}

static GnmSampler makeSampler(enum mPS4FilterMode filter) {
	GnmFilter mode = filter == M_PS4_FILTER_BILINEAR ?
		GNM_FILTER_BILINEAR : GNM_FILTER_POINT;
	const GnmSampler sampler = {
		.xyminfilter = mode,
		.xymagfilter = mode,
		.mipfilter = GNM_MIPFILTER_NONE,
		.zfilter = GNM_ZFILTER_POINT,
		.minlod = 0,
		.maxlod = 0,
	};
	return sampler;
}

static bool preflightSubmission(struct mPS4Renderer* renderer) {
	static const uint64_t expected = 0x4D4742415052464CULL;
	mPS4HardwareTrace("renderer: preflight begin", false);
	*renderer->label = 0;
	gnmCmdReset(&renderer->command);
	gnmDrawCmdInitDefaultHardwareState(&renderer->command);
	gnmDrawCmdDrawIndexAuto(&renderer->command, 0);
	gnmDrawCmdEventWriteEop(&renderer->command, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
		(uint64_t) renderer->label, GNM_DATA_SEL_SEND_DATA64, expected);

	GnmCommandBufferValidationInfo validation = {0};
	if (sceGnmCmdValidate(&renderer->command, &validation) != GNM_ERROR_OK) {
		mPS4HardwareTrace("renderer: preflight validation failed", false);
		return false;
	}
	mPS4HardwareTrace("renderer: preflight validated", false);
	void* commandAddress = renderer->command.beginptr;
	uint32_t commandSize = (uint32_t) ((renderer->command.cmdptr -
		renderer->command.beginptr) * sizeof(uint32_t));
	if (sceGnmSubmitCommandBuffers(1, &commandAddress, &commandSize, NULL, 0)) {
		mPS4HardwareTrace("renderer: preflight submit failed", false);
		return false;
	}
	mPS4HardwareTrace("renderer: preflight submitted", false);
	if (sceGnmSubmitDone()) {
		mPS4HardwareTrace("renderer: preflight submit done failed", false);
		return false;
	}
	mPS4HardwareTrace("renderer: preflight submit done", false);
	unsigned attempts = 0;
	while (*renderer->label != expected && attempts++ < 200000) sceKernelUsleep(50);
	if (*renderer->label != expected) {
		mPS4HardwareTrace("renderer: preflight EOP timeout", false);
		return false;
	}
	mPS4HardwareTrace("renderer: preflight EOP complete", false);
	return true;
}

static bool preflightShaderState(struct mPS4Renderer* renderer) {
	static const uint64_t expected = 0x4D47424153544154ULL;
	mPS4HardwareTrace("renderer: shader state preflight begin", false);
	*renderer->label = 0;
	gnmCmdReset(&renderer->command);
	gnmDrawCmdInitDefaultHardwareState(&renderer->command);

	const GnmPrimitiveSetup primitive = {
		.cullmode = GNM_CULL_NONE,
		.frontface = GNM_FACE_CCW,
		.frontmode = GNM_FILL_SOLID,
		.backmode = GNM_FILL_SOLID,
		.provokemode = GNM_PROVOKINGVTX_FIRST,
	};
	const GnmDbRenderControl dbControl = {0};
	const GnmDepthStencilControl depthControl = {0};
	gnmDrawCmdSetPrimitiveSetup(&renderer->command, &primitive);
	gnmDrawCmdSetDbRenderControl(&renderer->command, &dbControl);
	gnmDrawCmdSetDepthStencilControl(&renderer->command, &depthControl);
	gnmDrawCmdSetRenderTarget(&renderer->command, 0, &renderer->targets[0]);
	gnmDrawCmdSetRenderTargetMask(&renderer->command, 0xF);
	setupViewport(&renderer->command, renderer->outputWidth, renderer->outputHeight);
	gnmDrawCmdSetVsShader(&renderer->command, &renderer->vertexShader->registers, 0);
	gnmDrawCmdSetPsShader(&renderer->command, &renderer->pixelShader->registers);
	gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_VS, 0,
		renderer->fetchShader);
	gnmDrawCmdSetPsInputUsage(&renderer->command,
		gnmVsShaderExportSemanticTable(renderer->vertexShader),
		renderer->vertexShader->numexportsemantics,
		gnmPsShaderInputSemanticTable(renderer->pixelShader),
		renderer->pixelShader->numinputsemantics);
	gnmDrawCmdSetPrimitiveType(&renderer->command, GNM_PT_TRILIST);
	gnmDrawCmdDrawIndexAuto(&renderer->command, 0);
	gnmDrawCmdEventWriteEop(&renderer->command, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
		(uint64_t) renderer->label, GNM_DATA_SEL_SEND_DATA64, expected);

	GnmCommandBufferValidationInfo validation = {0};
	if (sceGnmCmdValidate(&renderer->command, &validation) != GNM_ERROR_OK) {
		mPS4HardwareTrace("renderer: shader state preflight validation failed", false);
		return false;
	}
	mPS4HardwareTrace("renderer: shader state preflight validated", false);
	void* commandAddress = renderer->command.beginptr;
	uint32_t commandSize = (uint32_t) ((renderer->command.cmdptr -
		renderer->command.beginptr) * sizeof(uint32_t));
	if (sceGnmSubmitCommandBuffers(1, &commandAddress, &commandSize, NULL, 0)) {
		mPS4HardwareTrace("renderer: shader state preflight submit failed", false);
		return false;
	}
	mPS4HardwareTrace("renderer: shader state preflight submitted", false);
	if (sceGnmSubmitDone()) {
		mPS4HardwareTrace("renderer: shader state preflight submit done failed", false);
		return false;
	}
	mPS4HardwareTrace("renderer: shader state preflight submit done", false);
	unsigned attempts = 0;
	while (*renderer->label != expected && attempts++ < 200000) sceKernelUsleep(50);
	if (*renderer->label != expected) {
		mPS4HardwareTrace("renderer: shader state preflight EOP timeout", false);
		return false;
	}
	mPS4HardwareTrace("renderer: shader state preflight EOP complete", false);
	return true;
}

static void setVertex(struct mPS4Vertex* vertex, float x, float y, float u, float v,
	struct mPS4Color color) {
	vertex->position[0] = x / (M_PS4_LOGICAL_WIDTH * 0.5f) - 1.0f;
	vertex->position[1] = 1.0f - y / (M_PS4_LOGICAL_HEIGHT * 0.5f);
	vertex->uv[0] = u;
	vertex->uv[1] = v;
	vertex->color[0] = color.r;
	vertex->color[1] = color.g;
	vertex->color[2] = color.b;
	vertex->color[3] = color.a;
}

bool mPS4RendererCreate(struct mPS4Renderer** outRenderer) {
	mPS4HardwareTrace("renderer: enter create", false);
	if (!outRenderer) return false;
	struct mPS4Renderer* renderer = calloc(1, sizeof(*renderer));
	if (!renderer) return false;
	renderer->direct = -1;
	renderer->video = -1;
	mPS4HardwareTrace("renderer: before UserService init", false);
	if (sceUserServiceInitialize(NULL) < 0) goto fail;
	mPS4HardwareTrace("renderer: after UserService init", false);
	mPS4HardwareTrace("renderer: before direct memory", false);
	if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), DIRECT_MEMORY_SIZE,
		DIRECT_MEMORY_ALIGNMENT, ORBIS_KERNEL_WC_GARLIC, &renderer->direct) ||
		sceKernelMapDirectMemory((void**) &renderer->base, DIRECT_MEMORY_SIZE,
			ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
			ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE,
			0, renderer->direct, DIRECT_MEMORY_ALIGNMENT)) goto fail;
	mPS4HardwareTrace("renderer: after direct memory", false);

	mPS4HardwareTrace("renderer: before VideoOut open", false);
	renderer->video = sceVideoOutOpen(0, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
	OrbisVideoOutResolutionStatus resolution = {0};
	if (renderer->video < 0 || sceVideoOutGetResolutionStatus(renderer->video, &resolution)) goto fail;
	mPS4HardwareTrace("renderer: after VideoOut resolution", false);
	renderer->outputWidth = resolution.width ? resolution.width : 1920;
	renderer->outputHeight = resolution.height ? resolution.height : 1080;
	for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
		mPS4HardwareTrace(i ? "renderer: before target 1" : "renderer: before target 0", false);
		if (!createTarget(renderer, &renderer->targets[i])) goto fail;
		mPS4HardwareTrace(i ? "renderer: after target 1" : "renderer: after target 0", false);
	}
	OrbisVideoOutBufferAttribute attribute = {0};
	sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
		ORBIS_VIDEO_OUT_TILING_MODE_TILE, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
		renderer->outputWidth, renderer->outputHeight, gnmRtGetPitch(&renderer->targets[0]));
	void* addresses[BUFFER_COUNT] = {
		gnmRtGetBaseAddr(&renderer->targets[0]), gnmRtGetBaseAddr(&renderer->targets[1])
	};
	mPS4HardwareTrace("renderer: before VideoOut buffer registration", false);
	if (sceVideoOutRegisterBuffers(renderer->video, 0, addresses, BUFFER_COUNT, &attribute) < 0 ||
		sceKernelCreateEqueue(&renderer->flips, "mgba ps4 renderer flips") ||
		sceVideoOutAddFlipEvent(renderer->flips, renderer->video, NULL) ||
		sceVideoOutSetFlipRate(renderer->video, ORBIS_VIDEO_OUT_FLIP_60HZ)) goto fail;
	mPS4HardwareTrace("renderer: after VideoOut buffer registration", false);

	mPS4HardwareTrace("renderer: before textures", false);
	for (unsigned i = 0; i < M_PS4_FRAME_TEXTURE_COUNT; ++i) {
		if (!createTexture(renderer, i, M_PS4_FRAME_TEXTURE_WIDTH,
			M_PS4_FRAME_TEXTURE_HEIGHT, NULL, M_PS4_FRAME_TEXTURE_WIDTH)) goto fail;
	}
	renderer->textureCount = M_PS4_FRAME_TEXTURE_COUNT;
	const uint32_t black = 0xFF000000;
	if (!mPS4RendererCreateTexture(renderer, 1, 1, &black, 1, NULL)) goto fail;
	mPS4HardwareTrace("renderer: after textures", false);
	mPS4HardwareTrace("renderer: before command memory", false);
	void* commandMemory = allocGarlic(renderer, COMMAND_BUFFER_SIZE, GNM_ALIGNMENT_BUFFER_BYTES);
	renderer->vertexMemory = allocGarlic(renderer,
		MAX_QUADS * VERTICES_PER_QUAD * sizeof(*renderer->vertexMemory),
		GNM_ALIGNMENT_BUFFER_BYTES);
	renderer->label = allocGarlic(renderer, sizeof(*renderer->label), sizeof(*renderer->label));
	if (!commandMemory || !renderer->vertexMemory || !renderer->label) goto fail;
	renderer->command = gnmCmdInit(commandMemory, COMMAND_BUFFER_SIZE, NULL, NULL);
	mPS4HardwareTrace("renderer: after command memory", false);
	if (!preflightSubmission(renderer)) goto fail;
	mPS4HardwareTrace("renderer: before vertex shader", false);
	if (!loadVertexShader(renderer, "/app0/assets/misc/quad.vert.sb", 2, 1,
		&renderer->vertexShader)) goto fail;
	mPS4HardwareTrace("renderer: after vertex shader", false);
	mPS4HardwareTrace("renderer: before pixel shader", false);
	if (!loadPixelShader(renderer, "/app0/assets/misc/quad.frag.sb", 1,
		&renderer->pixelShader)) goto fail;
	mPS4HardwareTrace("renderer: after pixel shader", false);
	mPS4HardwareTrace("renderer: before fetch shader", false);
	if (!createFetchShader(renderer)) goto fail;
	mPS4HardwareTrace("renderer: after fetch shader", false);
	if (!preflightShaderState(renderer)) goto fail;
	*outRenderer = renderer;
	mPS4HardwareTrace("renderer: create complete", false);
	return true;

fail:
	mPS4HardwareTrace("renderer: create failed", false);
	mPS4RendererDestroy(renderer);
	return false;
}

void mPS4RendererDestroy(struct mPS4Renderer* renderer) {
	if (!renderer) return;
	free(renderer->vertexShader);
	free(renderer->pixelShader);
	if (renderer->video >= 0) sceVideoOutClose(renderer->video);
	if (renderer->flips) sceKernelDeleteEqueue(renderer->flips);
	if (renderer->direct >= 0) sceKernelReleaseDirectMemory(renderer->direct, DIRECT_MEMORY_SIZE);
	sceUserServiceTerminate();
	free(renderer);
}

bool mPS4RendererRunReferenceTriangle(struct mPS4Renderer* renderer) {
	static const uint64_t expected = 0x4D47424154524937ULL;
	if (!renderer) return false;
	mPS4HardwareTrace("renderer: reference triangle begin", false);
	GnmVsShader* vertexShader = NULL;
	GnmPsShader* pixelShader = NULL;
	if (!loadVertexShader(renderer, "/app0/assets/misc/tri.vert.sb", 0, 1,
			&vertexShader) ||
		!loadPixelShader(renderer, "/app0/assets/misc/tri.frag.sb", 1, &pixelShader)) {
		mPS4HardwareTrace("renderer: reference triangle shader load failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference triangle shaders loaded", false);

	*renderer->label = 0;
	gnmCmdReset(&renderer->command);
	gnmDrawCmdInitDefaultHardwareState(&renderer->command);
	const GnmPrimitiveSetup primitive = {
		.cullmode = GNM_CULL_NONE,
		.frontface = GNM_FACE_CCW,
		.frontmode = GNM_FILL_SOLID,
		.backmode = GNM_FILL_SOLID,
		.provokemode = GNM_PROVOKINGVTX_FIRST,
	};
	const GnmDbRenderControl dbControl = {0};
	const GnmDepthStencilControl depthControl = {0};
	gnmDrawCmdSetPrimitiveSetup(&renderer->command, &primitive);
	gnmDrawCmdSetDbRenderControl(&renderer->command, &dbControl);
	gnmDrawCmdSetDepthStencilControl(&renderer->command, &depthControl);
	gnmDrawCmdSetRenderTarget(&renderer->command, 0, &renderer->targets[0]);
	gnmDrawCmdSetRenderTargetMask(&renderer->command, 0xF);
	setupViewport(&renderer->command, renderer->outputWidth, renderer->outputHeight);
	gnmDrawCmdSetVsShader(&renderer->command, &vertexShader->registers, 0);
	gnmDrawCmdSetPsShader(&renderer->command, &pixelShader->registers);
	gnmDrawCmdSetPsInputUsage(&renderer->command,
		gnmVsShaderExportSemanticTable(vertexShader), vertexShader->numexportsemantics,
		gnmPsShaderInputSemanticTable(pixelShader), pixelShader->numinputsemantics);
	gnmDrawCmdSetPrimitiveType(&renderer->command, GNM_PT_TRILIST);
	gnmDrawCmdDrawIndexAuto(&renderer->command, 3);
	gnmDrawCmdEventWriteEop(&renderer->command, GNM_CACHE_FLUSH_TS,
		(uint64_t) renderer->label, GNM_DATA_SEL_SEND_DATA64, expected);

	GnmCommandBufferValidationInfo validation = {0};
	if (sceGnmCmdValidate(&renderer->command, &validation) != GNM_ERROR_OK) {
		mPS4HardwareTrace("renderer: reference triangle validation failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference triangle validated", false);
	void* commandAddress = renderer->command.beginptr;
	uint32_t commandSize = (uint32_t) ((renderer->command.cmdptr -
		renderer->command.beginptr) * sizeof(uint32_t));
	if (sceGnmSubmitCommandBuffers(1, &commandAddress, &commandSize, NULL, 0)) {
		mPS4HardwareTrace("renderer: reference triangle submit failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference triangle submitted", false);
	unsigned attempts = 0;
	while (*renderer->label != expected && attempts++ < 200000) sceKernelUsleep(50);
	if (*renderer->label != expected) {
		mPS4HardwareTrace("renderer: reference triangle EOP timeout", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference triangle EOP complete", false);
	if (sceVideoOutSubmitFlip(renderer->video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 1)) {
		mPS4HardwareTrace("renderer: reference triangle flip failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	OrbisKernelEvent event = {0};
	int count = 0;
	if (sceKernelWaitEqueue(renderer->flips, &event, 1, &count, 0)) {
		mPS4HardwareTrace("renderer: reference triangle flip wait failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference triangle flip complete", false);
	if (sceGnmSubmitDone()) {
		mPS4HardwareTrace("renderer: reference triangle submit done failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference triangle complete", false);
	free(vertexShader);
	free(pixelShader);
	return true;
}

bool mPS4RendererRunReferenceBlit(struct mPS4Renderer* renderer) {
	static const uint64_t expected = 0x4D474241424C5438ULL;
	static const struct mPS4BlitVertex sourceVertices[] = {
		{{-1.0f, -1.0f}, {0.0f, 1.0f}},
		{{ 1.0f, -1.0f}, {1.0f, 1.0f}},
		{{ 1.0f,  1.0f}, {1.0f, 0.0f}},
		{{-1.0f, -1.0f}, {0.0f, 1.0f}},
		{{ 1.0f,  1.0f}, {1.0f, 0.0f}},
		{{-1.0f,  1.0f}, {0.0f, 0.0f}},
	};
	if (!renderer) return false;
	mPS4HardwareTrace("renderer: reference blit begin", false);
	GnmVsShader* vertexShader = NULL;
	GnmPsShader* pixelShader = NULL;
	void* fetchShader = NULL;
	if (!loadVertexShader(renderer, "/app0/assets/misc/quad.vert.sb", 2, 1,
			&vertexShader) ||
		!loadPixelShader(renderer, "/app0/assets/misc/quad.frag.sb", 1, &pixelShader) ||
		!createFetchShaderFor(renderer, vertexShader, &fetchShader)) {
		mPS4HardwareTrace("renderer: reference blit shader setup failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference blit shaders loaded", false);

	struct mPS4BlitVertex* vertices = allocGarlic(renderer, sizeof(sourceVertices),
		GNM_ALIGNMENT_BUFFER_BYTES);
	GnmBuffer* buffers = allocGarlic(renderer, sizeof(GnmBuffer) * 2,
		GNM_ALIGNMENT_BUFFER_BYTES);
	struct mPS4PsDescriptors* descriptors = allocGarlic(renderer, sizeof(*descriptors),
		GNM_ALIGNMENT_BUFFER_BYTES);
	if (!vertices || !buffers || !descriptors) {
		mPS4HardwareTrace("renderer: reference blit resource allocation failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	memcpy(vertices, sourceVertices, sizeof(sourceVertices));
	buffers[0] = gnmCreateVertexBuffer(vertices, GNM_FMT_R32G32_FLOAT,
		sizeof(*vertices), 6);
	buffers[1] = gnmCreateVertexBuffer((uint8_t*) vertices +
		offsetof(struct mPS4BlitVertex, uv), GNM_FMT_R32G32_FLOAT,
		sizeof(*vertices), 6);
	for (uint32_t y = 0; y < M_PS4_FRAME_TEXTURE_HEIGHT; ++y) {
		for (uint32_t x = 0; x < M_PS4_FRAME_TEXTURE_WIDTH; ++x) {
			uint8_t red = (uint8_t) x;
			uint8_t green = (uint8_t) y;
			uint8_t blue = ((x / 16 + y / 16) & 1) ? 0xE0 : 0x20;
			renderer->texturePixels[0][y * M_PS4_FRAME_TEXTURE_WIDTH + x] =
				(uint32_t) red | ((uint32_t) green << 8) |
				((uint32_t) blue << 16) | 0xFF000000U;
		}
	}
	descriptors->texture = renderer->textures[0];
	descriptors->sampler = makeSampler(M_PS4_FILTER_NEAREST);
	mPS4HardwareTrace("renderer: reference blit resources ready", false);

	*renderer->label = 0;
	gnmCmdReset(&renderer->command);
	gnmDrawCmdInitDefaultHardwareState(&renderer->command);
	const GnmPrimitiveSetup primitive = {
		.cullmode = GNM_CULL_NONE,
		.frontface = GNM_FACE_CCW,
		.frontmode = GNM_FILL_SOLID,
		.backmode = GNM_FILL_SOLID,
		.provokemode = GNM_PROVOKINGVTX_FIRST,
	};
	const GnmDbRenderControl dbControl = {0};
	const GnmDepthStencilControl depthControl = {0};
	gnmDrawCmdSetPrimitiveSetup(&renderer->command, &primitive);
	gnmDrawCmdSetDbRenderControl(&renderer->command, &dbControl);
	gnmDrawCmdSetDepthStencilControl(&renderer->command, &depthControl);
	gnmDrawCmdSetRenderTarget(&renderer->command, 0, &renderer->targets[0]);
	gnmDrawCmdSetRenderTargetMask(&renderer->command, 0xF);
	setupViewport(&renderer->command, renderer->outputWidth, renderer->outputHeight);
	gnmDrawCmdSetVsShader(&renderer->command, &vertexShader->registers, 0);
	gnmDrawCmdSetPsShader(&renderer->command, &pixelShader->registers);
	gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_VS, 0, fetchShader);
	gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_VS, 2, buffers);
	gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_PS, 0, descriptors);
	gnmDrawCmdSetPsInputUsage(&renderer->command,
		gnmVsShaderExportSemanticTable(vertexShader), vertexShader->numexportsemantics,
		gnmPsShaderInputSemanticTable(pixelShader), pixelShader->numinputsemantics);
	gnmDrawCmdSetPrimitiveType(&renderer->command, GNM_PT_TRILIST);
	gnmDrawCmdDrawIndexAuto(&renderer->command, 6);
	gnmDrawCmdEventWriteEop(&renderer->command, GNM_CACHE_FLUSH_TS,
		(uint64_t) renderer->label, GNM_DATA_SEL_SEND_DATA64, expected);

	GnmCommandBufferValidationInfo validation = {0};
	if (sceGnmCmdValidate(&renderer->command, &validation) != GNM_ERROR_OK) {
		mPS4HardwareTrace("renderer: reference blit validation failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference blit validated", false);
	void* commandAddress = renderer->command.beginptr;
	uint32_t commandSize = (uint32_t) ((renderer->command.cmdptr -
		renderer->command.beginptr) * sizeof(uint32_t));
	if (sceGnmSubmitCommandBuffers(1, &commandAddress, &commandSize, NULL, 0)) {
		mPS4HardwareTrace("renderer: reference blit submit failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference blit submitted", false);
	unsigned attempts = 0;
	while (*renderer->label != expected && attempts++ < 200000) sceKernelUsleep(50);
	if (*renderer->label != expected) {
		mPS4HardwareTrace("renderer: reference blit EOP timeout", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference blit EOP complete", false);
	if (sceVideoOutSubmitFlip(renderer->video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 1)) {
		mPS4HardwareTrace("renderer: reference blit flip failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	OrbisKernelEvent event = {0};
	int count = 0;
	if (sceKernelWaitEqueue(renderer->flips, &event, 1, &count, 0)) {
		mPS4HardwareTrace("renderer: reference blit flip wait failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference blit flip complete", false);
	if (sceGnmSubmitDone()) {
		mPS4HardwareTrace("renderer: reference blit submit done failed", false);
		free(vertexShader);
		free(pixelShader);
		return false;
	}
	mPS4HardwareTrace("renderer: reference blit complete", false);
	free(vertexShader);
	free(pixelShader);
	return true;
}

uint32_t* mPS4RendererFramePixels(struct mPS4Renderer* renderer,
	unsigned textureIndex, uint32_t* stridePixels) {
	if (!renderer || textureIndex >= M_PS4_FRAME_TEXTURE_COUNT) return NULL;
	if (stridePixels) *stridePixels = M_PS4_FRAME_TEXTURE_WIDTH;
	return renderer->texturePixels[textureIndex];
}

bool mPS4RendererCreateTexture(struct mPS4Renderer* renderer, uint32_t width,
	uint32_t height, const uint32_t* pixels, uint32_t stridePixels,
	unsigned* textureIndex) {
	if (!renderer || !width || !height || width > 8192 || height > 8192 ||
		renderer->textureCount >= M_PS4_MAX_TEXTURES || (pixels && stridePixels < width)) {
		return false;
	}
	unsigned index = renderer->textureCount;
	if (!createTexture(renderer, index, width, height, pixels,
		pixels ? stridePixels : width)) {
		return false;
	}
	renderer->textureCount++;
	if (textureIndex) *textureIndex = index;
	return true;
}

bool mPS4RendererBegin(struct mPS4Renderer* renderer) {
	if (!renderer || renderer->frameOpen) return false;
	renderer->quadCount = 0;
	renderer->frameOpen = true;
	return true;
}

void mPS4RendererCancel(struct mPS4Renderer* renderer) {
	if (renderer) renderer->frameOpen = false;
}

bool mPS4RendererAddQuad(struct mPS4Renderer* renderer, unsigned textureIndex,
	struct mPS4Rect destination, struct mPS4UvRect uv, struct mPS4Color color,
	enum mPS4FilterMode filter) {
	if (!renderer || !renderer->frameOpen || textureIndex >= renderer->textureCount ||
		renderer->quadCount >= MAX_QUADS || !destination.width || !destination.height) return false;
	struct mPS4Quad* quad = &renderer->quads[renderer->quadCount++];
	float left = (float) destination.x;
	float top = (float) destination.y;
	float right = (float) (destination.x + destination.width);
	float bottom = (float) (destination.y + destination.height);
	setVertex(&quad->vertices[0], left, top, uv.left, uv.top, color);
	setVertex(&quad->vertices[1], right, top, uv.right, uv.top, color);
	setVertex(&quad->vertices[2], right, bottom, uv.right, uv.bottom, color);
	setVertex(&quad->vertices[3], left, top, uv.left, uv.top, color);
	setVertex(&quad->vertices[4], right, bottom, uv.right, uv.bottom, color);
	setVertex(&quad->vertices[5], left, bottom, uv.left, uv.bottom, color);
	quad->texture = textureIndex;
	quad->filter = filter;
	return true;
}

bool mPS4RendererEnd(struct mPS4Renderer* renderer) {
	if (!renderer || !renderer->frameOpen || !renderer->quadCount) return false;
	renderer->frameOpen = false;
	renderer->frame++;
	bool traceFirstFrame = renderer->frame == 1;
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame begin", false);
	unsigned targetIndex = (unsigned) (renderer->frame % BUFFER_COUNT);
	*renderer->label = 0;
	gnmCmdReset(&renderer->command);
	gnmDrawCmdInitDefaultHardwareState(&renderer->command);
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame default state", false);

	const GnmPrimitiveSetup primitive = {
		.cullmode = GNM_CULL_NONE,
		.frontface = GNM_FACE_CCW,
		.frontmode = GNM_FILL_SOLID,
		.backmode = GNM_FILL_SOLID,
		.provokemode = GNM_PROVOKINGVTX_FIRST,
	};
	GnmBlendControl blend = {
		.blendenabled = true,
		.colorfunc = GNM_COMB_DST_PLUS_SRC,
		.colorsrcmult = GNM_BLEND_SRC_ALPHA,
		.colordstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA,
		.separatealphaenable = false,
	};
	const GnmDbRenderControl dbControl = {0};
	const GnmDepthStencilControl depthControl = {0};
	gnmDrawCmdSetPrimitiveSetup(&renderer->command, &primitive);
	gnmDrawCmdSetDbRenderControl(&renderer->command, &dbControl);
	gnmDrawCmdSetDepthStencilControl(&renderer->command, &depthControl);
	gnmDrawCmdSetRenderTarget(&renderer->command, 0, &renderer->targets[targetIndex]);
	gnmDrawCmdSetRenderTargetMask(&renderer->command, 0xF);
	setupViewport(&renderer->command, renderer->outputWidth, renderer->outputHeight);
	gnmDrawCmdSetVsShader(&renderer->command, &renderer->vertexShader->registers, 0);
	gnmDrawCmdSetPsShader(&renderer->command, &renderer->pixelShader->registers);
	gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_VS, 0, renderer->fetchShader);
	gnmDrawCmdSetPsInputUsage(&renderer->command,
		gnmVsShaderExportSemanticTable(renderer->vertexShader),
		renderer->vertexShader->numexportsemantics,
		gnmPsShaderInputSemanticTable(renderer->pixelShader),
		renderer->pixelShader->numinputsemantics);
	gnmDrawCmdSetPrimitiveType(&renderer->command, GNM_PT_TRILIST);

	for (unsigned i = 0; i < renderer->quadCount; ++i) {
		blend.blendenabled = renderer->quads[i].texture >= M_PS4_FRAME_TEXTURE_COUNT;
		gnmDrawCmdSetBlendControl(&renderer->command, 0, &blend);
		struct mPS4Vertex* vertices = renderer->vertexMemory + i * VERTICES_PER_QUAD;
		memcpy(vertices, renderer->quads[i].vertices, sizeof(renderer->quads[i].vertices));
		GnmBuffer* buffers = gnmCmdAllocInside(&renderer->command, sizeof(GnmBuffer) * 2, 4);
		struct mPS4PsDescriptors* descriptors = gnmCmdAllocInside(&renderer->command,
			sizeof(*descriptors), 4);
		if (!buffers || !descriptors) return false;
		buffers[0] = gnmCreateVertexBuffer(vertices, GNM_FMT_R32G32_FLOAT,
			sizeof(*vertices), VERTICES_PER_QUAD);
		buffers[1] = gnmCreateVertexBuffer((uint8_t*) vertices + offsetof(struct mPS4Vertex, uv),
			GNM_FMT_R32G32_FLOAT, sizeof(*vertices), VERTICES_PER_QUAD);
		descriptors->texture = renderer->textures[renderer->quads[i].texture];
		descriptors->sampler = makeSampler(renderer->quads[i].filter);
		gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_VS, 2, buffers);
		gnmDrawCmdSetPointerUserData(&renderer->command, GNM_STAGE_PS, 0, descriptors);
		gnmDrawCmdDrawIndexAuto(&renderer->command, VERTICES_PER_QUAD);
	}
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame quads emitted", false);

	gnmDrawCmdEventWriteEop(&renderer->command, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
		(uint64_t) renderer->label, GNM_DATA_SEL_SEND_DATA64, renderer->frame);
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame before validation", false);
	GnmCommandBufferValidationInfo validation = {0};
	if (sceGnmCmdValidate(&renderer->command, &validation) != GNM_ERROR_OK) {
		printf("ps4-renderer: invalid command buffer: %s\n",
			validation.message ? validation.message : "unknown error");
		return false;
	}
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame after validation", false);
	void* commandAddress = renderer->command.beginptr;
	uint32_t commandSize = (uint32_t) ((renderer->command.cmdptr -
		renderer->command.beginptr) * sizeof(uint32_t));
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame before submit", false);
	if (sceGnmSubmitCommandBuffers(1, &commandAddress, &commandSize, NULL, 0)) return false;
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame after submit", false);
	unsigned attempts = 0;
	while (*renderer->label != renderer->frame && attempts++ < 200000) sceKernelUsleep(50);
	if (*renderer->label != renderer->frame) {
		if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame EOP timeout", false);
		return false;
	}
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame EOP complete", false);
	if (sceVideoOutSubmitFlip(renderer->video, (int) targetIndex,
		ORBIS_VIDEO_OUT_FLIP_VSYNC, (int64_t) renderer->frame)) return false;
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame flip submitted", false);
	OrbisKernelEvent event = {0};
	int count = 0;
	if (sceKernelWaitEqueue(renderer->flips, &event, 1, &count, 0)) return false;
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame flip complete", false);
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame before submit done", false);
	if (sceGnmSubmitDone()) {
		if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame submit done failed", false);
		return false;
	}
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame after submit done", false);
	if (traceFirstFrame) mPS4HardwareTrace("renderer: first frame complete", false);
	return true;
}

bool mPS4RendererPresentFrame(struct mPS4Renderer* renderer, unsigned textureIndex,
	uint32_t sourceWidth, uint32_t sourceHeight, enum mPS4ScreenMode screenMode,
	enum mPS4FilterMode filter) {
	if (!mPS4RendererBegin(renderer)) return false;
	const struct mPS4UvRect fullUv = { 0.0f, 0.0f, 1.0f, 1.0f };
	const struct mPS4Color black = { 0.0f, 0.0f, 0.0f, 1.0f };
	const struct mPS4Color white = { 1.0f, 1.0f, 1.0f, 1.0f };
	const struct mPS4Rect background = { 0, 0, M_PS4_LOGICAL_WIDTH, M_PS4_LOGICAL_HEIGHT };
	struct mPS4Rect destination = mPS4OutputRect(sourceWidth, sourceHeight,
		M_PS4_LOGICAL_WIDTH, M_PS4_LOGICAL_HEIGHT, screenMode);
	struct mPS4UvRect source = {
		0.0f, 0.0f,
		(float) sourceWidth / M_PS4_FRAME_TEXTURE_WIDTH,
		(float) sourceHeight / M_PS4_FRAME_TEXTURE_HEIGHT,
	};
	return mPS4RendererAddQuad(renderer, M_PS4_SOLID_TEXTURE, background, fullUv, black, filter) &&
		mPS4RendererAddQuad(renderer, textureIndex, destination, source, white, filter) &&
		mPS4RendererEnd(renderer);
}
