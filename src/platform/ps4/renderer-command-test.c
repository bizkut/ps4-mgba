/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <gnm_commandbuffer.h>
#include <gnm_drawcommandbuffer.h>
#include <gnm_helpers.h>
#include <gnm_rendertarget.h>
#include <gnm_shaderbinary.h>
#include <gnm_texture.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

static void validateShader(const char* path, GnmShaderType expected) {
	FILE* file = fopen(path, "rb");
	CHECK(file);
	CHECK(!fseek(file, 0, SEEK_END));
	long size = ftell(file);
	CHECK(size > 0);
	CHECK(!fseek(file, 0, SEEK_SET));
	void* data = malloc((size_t) size);
	CHECK(data);
	CHECK(fread(data, 1, (size_t) size, file) == (size_t) size);
	fclose(file);
	GnmShaderMetadata metadata = {0};
	CHECK(sceGnmShaderBinaryGetMetadata(data, (size_t) size, &metadata) == GNM_ERROR_OK);
	CHECK(metadata.type == expected);
	CHECK(metadata.stage && metadata.stagesize);
	CHECK(metadata.shadercode && metadata.shadercodesize);
	if (expected == GNM_SHADER_VERTEX) {
		CHECK(metadata.numinputsemantics == 3);
		CHECK(metadata.numexportsemantics == 2);
	} else {
		CHECK(metadata.numinputsemantics == 2);
	}
	free(data);
}

int main(int argc, char** argv) {
	uint32_t textureMemory[256 * 256] = {0};
	GnmTexture texture = {0};
	GnmTextureCreateInfo textureInfo = {0};
	sceGnmTexInit2dCreateInfo(&textureInfo, GNM_FMT_R8G8B8A8_SRGB, 256, 256, 1,
		GNM_TM_DISPLAY_LINEAR_GENERAL, GNM_GPU_BASE);
	CHECK(sceGnmCreateTexture(&texture, &textureInfo) == GNM_ERROR_OK);
	sceGnmTexSetBaseAddress(&texture, textureMemory);
	CHECK(sceGnmTexGetPitch(&texture) == 256);
	CHECK(sceGnmTexGetWidth(&texture) == 256);
	CHECK(sceGnmTexGetHeight(&texture) == 256);

	GnmRenderTarget target = {0};
	GnmRenderTargetCreateInfo targetInfo = {0};
	sceGnmRtInitColorTargetCreateInfo(&targetInfo, GNM_FMT_R8G8B8A8_SRGB,
		1280, 720, 1, 1, 1, GNM_TM_DISPLAY_LINEAR_ALIGNED, GNM_GPU_BASE);
	CHECK(sceGnmCreateRenderTarget(&target, &targetInfo) == GNM_ERROR_OK);

	uint32_t commandMemory[4096] = {0};
	GnmCommandBuffer command = sceGnmCmdInit(commandMemory, sizeof(commandMemory), NULL, NULL);
	sceGnmDrawCmdInitDefaultHardwareState(&command);
	sceGnmDrawCmdSetRenderTarget(&command, 0, &target);
	sceGnmDrawCmdSetRenderTargetMask(&command, 0xF);
	sceGnmDrawCmdSetPrimitiveType(&command, GNM_PT_TRILIST);
	sceGnmDrawCmdDrawIndexAuto(&command, 6);
	GnmCommandBufferValidationInfo validation = {0};
	CHECK(sceGnmCmdValidate(&command, &validation) == GNM_ERROR_OK);
	CHECK(validation.useddwords > 0);

	if (argc == 3) {
		validateShader(argv[1], GNM_SHADER_VERTEX);
		validateShader(argv[2], GNM_SHADER_PIXEL);
	}
	puts("PS4 OpenGNM descriptor and command validation passed");
	return 0;
}
