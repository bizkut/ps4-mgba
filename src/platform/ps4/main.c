/* Copyright (c) 2026 mGBA PS4 port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-font.h"
#include "renderer.h"

#include "feature/gui/gui-runner.h"
#include <mgba/core/core.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>
#include <mgba-util/threading.h>

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PS4_INPUT 0x50533431

struct mPS4SystemServiceEvent {
	int32_t eventType;
	uint8_t payload[8192];
};

extern int32_t sceSystemServiceReceiveEvent(struct mPS4SystemServiceEvent* event);

enum {
	AUDIO_PACKET_COUNT = 4,
	AUDIO_PACKET_FRAMES = 1024,
};

static const uint64_t FRAME_INTERVAL_US = (1000000ULL + 59) / 60;

struct mPS4AudioPacket {
	int16_t samples[AUDIO_PACKET_FRAMES * 2] __attribute__((aligned(64)));
	bool full;
};

static struct {
	struct mPS4AudioPacket packets[AUDIO_PACKET_COUNT];
	unsigned readPacket;
	unsigned writePacket;
	struct mAudioBuffer buffer;
	struct mAudioResampler resampler;
	Mutex mutex;
	Condition cond;
	Thread thread;
	bool initialized;
	bool running;
	bool threadStarted;
} s_audio;
static struct mAVStream s_stream;
static int32_t s_user = -1;

static struct mPS4Renderer* s_renderer;
static int s_pad = -1;
static unsigned s_frameTexture;
static enum mPS4ScreenMode s_screenMode = M_PS4_SCREEN_ASPECT_FIT;
static enum mPS4FilterMode s_filterMode = M_PS4_FILTER_NEAREST;
static bool s_running = true;
static bool s_frameLimiter = true;
static uint64_t s_lastFrameTime;
static unsigned s_unthrottledFrames;
static uint64_t s_pacerSampleStart;
static unsigned s_pacerSampleFrames;
static struct mRumbleIntegrator s_rumble;

static void startupLog(const char* message, const char* mode);

static THREAD_ENTRY audioThread(void* context) {
	UNUSED(context);
	int port = sceAudioOutOpen(s_user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
		AUDIO_PACKET_FRAMES, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
	if (port < 0) {
		MutexLock(&s_audio.mutex);
		s_audio.running = false;
		ConditionWake(&s_audio.cond);
		MutexUnlock(&s_audio.mutex);
		THREAD_EXIT(0);
	}
	for (;;) {
		MutexLock(&s_audio.mutex);
		struct mPS4AudioPacket* packet = &s_audio.packets[s_audio.readPacket];
		while (s_audio.running && !packet->full) {
			ConditionWait(&s_audio.cond, &s_audio.mutex);
			packet = &s_audio.packets[s_audio.readPacket];
		}
		if (!s_audio.running) {
			MutexUnlock(&s_audio.mutex);
			break;
		}
		MutexUnlock(&s_audio.mutex);
		sceAudioOutOutput(port, packet->samples);
		MutexLock(&s_audio.mutex);
		packet->full = false;
		s_audio.readPacket = (s_audio.readPacket + 1) % AUDIO_PACKET_COUNT;
		ConditionWake(&s_audio.cond);
		MutexUnlock(&s_audio.mutex);
	}
	sceAudioOutClose(port);
	THREAD_EXIT(0);
}

static void postAudioBuffer(struct mAVStream* stream, struct mAudioBuffer* source) {
	UNUSED(stream);
	UNUSED(source);
	if (!s_audio.initialized) return;
	MutexLock(&s_audio.mutex);
	mAudioResamplerProcess(&s_audio.resampler);
	while (mAudioBufferAvailable(&s_audio.buffer) >= AUDIO_PACKET_FRAMES) {
		struct mPS4AudioPacket* packet = &s_audio.packets[s_audio.writePacket];
		while (packet->full && s_audio.running && s_frameLimiter) {
			ConditionWait(&s_audio.cond, &s_audio.mutex);
		}
		if (!s_audio.running) break;
		if (packet->full) {
			int16_t dropped[AUDIO_PACKET_FRAMES * 2];
			mAudioBufferRead(&s_audio.buffer, dropped, AUDIO_PACKET_FRAMES);
			continue;
		}
		mAudioBufferRead(&s_audio.buffer, packet->samples, AUDIO_PACKET_FRAMES);
		packet->full = true;
		s_audio.writePacket = (s_audio.writePacket + 1) % AUDIO_PACKET_COUNT;
		ConditionWake(&s_audio.cond);
	}
	MutexUnlock(&s_audio.mutex);
}

static void audioRateChanged(struct mAVStream* stream, unsigned sampleRate) {
	UNUSED(stream);
	if (!s_audio.initialized || !sampleRate) return;
	MutexLock(&s_audio.mutex);
	mAudioResamplerProcess(&s_audio.resampler);
	mAudioResamplerSetSource(&s_audio.resampler, s_audio.resampler.source,
		sampleRate, true);
	MutexUnlock(&s_audio.mutex);
}

static void clearAudioPackets(void) {
	if (!s_audio.initialized) return;
	MutexLock(&s_audio.mutex);
	mAudioBufferClear(&s_audio.buffer);
	for (unsigned i = 0; i < AUDIO_PACKET_COUNT; ++i) s_audio.packets[i].full = false;
	s_audio.readPacket = 0;
	s_audio.writePacket = 0;
	ConditionWake(&s_audio.cond);
	MutexUnlock(&s_audio.mutex);
}

static void stopAudio(void) {
	if (!s_audio.initialized) return;
	MutexLock(&s_audio.mutex);
	s_audio.running = false;
	ConditionWake(&s_audio.cond);
	MutexUnlock(&s_audio.mutex);
	if (s_audio.threadStarted) ThreadJoin(&s_audio.thread);
	mAudioResamplerDeinit(&s_audio.resampler);
	mAudioBufferDeinit(&s_audio.buffer);
	ConditionDeinit(&s_audio.cond);
	MutexDeinit(&s_audio.mutex);
	memset(&s_audio, 0, sizeof(s_audio));
}

static void setRumble(struct mRumbleIntegrator* source, float level) {
	UNUSED(source);
	if (s_pad < 0) return;
	if (level < 0.0f) level = 0.0f;
	if (level > 1.0f) level = 1.0f;
	const OrbisPadVibeParam vibration = {
		.lgMotor = (uint8_t) (level * level * 255.0f),
		.smMotor = (uint8_t) (level * 255.0f),
	};
	scePadSetVibration(s_pad, &vibration);
}

static void stopRumble(void) {
	setRumble(&s_rumble, 0.0f);
	mRumbleIntegratorReset(&s_rumble);
}

static void mapKey(struct mInputMap* map, uint32_t nativeKey, int key) {
	mInputBindKey(map, PS4_INPUT, __builtin_ctz(nativeKey), key);
}

static uint32_t readPad(const struct mInputMap* map) {
	if (s_pad < 0) return 0;
	OrbisPadData data = {0};
	if (scePadReadState(s_pad, &data) < 0 || !data.connected) return 0;
	return mInputMapKeyBits(map, PS4_INPUT, data.buttons, 0);
}

static void drawStart(void) {
	const struct mPS4Rect screen = { 0, 0, M_PS4_LOGICAL_WIDTH, M_PS4_LOGICAL_HEIGHT };
	const struct mPS4UvRect whitePixel = { 0.0f, 0.0f, 1.0f, 1.0f };
	const struct mPS4Color black = { 0.0f, 0.0f, 0.0f, 1.0f };
	if (!mPS4RendererBegin(s_renderer) ||
		!mPS4RendererAddQuad(s_renderer, M_PS4_SOLID_TEXTURE, screen, whitePixel,
			black, M_PS4_FILTER_NEAREST)) {
		s_running = false;
	}
}

static void waitForFrameCap(void) {
	uint64_t now = sceKernelGetProcessTime();
	if (s_lastFrameTime && now >= s_lastFrameTime) {
		uint64_t elapsed = now - s_lastFrameTime;
		if (elapsed < FRAME_INTERVAL_US) {
			sceKernelUsleep((uint32_t) (FRAME_INTERVAL_US - elapsed));
		}
	}
	s_lastFrameTime = sceKernelGetProcessTime();
}

static void recordPacerSample(void) {
	uint64_t now = sceKernelGetProcessTime();
	if (!s_pacerSampleStart) s_pacerSampleStart = now;
	if (now < s_pacerSampleStart) {
		s_pacerSampleStart = now;
		s_pacerSampleFrames = 0;
	}
	++s_pacerSampleFrames;
	uint64_t elapsed = now - s_pacerSampleStart;
	if (elapsed < 1000000) return;
	char message[128];
	snprintf(message, sizeof(message), "pacer: %s frames=%u elapsed_us=%llu",
		s_frameLimiter ? "normal" : "fast-forward", s_pacerSampleFrames,
		(unsigned long long) elapsed);
	startupLog(message, "a");
	s_pacerSampleStart = now;
	s_pacerSampleFrames = 0;
}

static void drawEnd(void) {
	if (!s_frameLimiter && ++s_unthrottledFrames % 4) {
		mPS4RendererCancel(s_renderer);
		recordPacerSample();
		return;
	}
	if (!mPS4RendererEnd(s_renderer)) {
		s_running = false;
		return;
	}
	if (s_frameLimiter) waitForFrameCap();
	recordPacerSample();
}

static uint32_t pollInput(const struct mInputMap* map) {
	return readPad(map);
}

static enum GUICursorState pollCursor(unsigned* x, unsigned* y) {
	UNUSED(x);
	UNUSED(y);
	return GUI_CURSOR_NOT_PRESENT;
}

static int batteryState(void) {
	return BATTERY_NOT_PRESENT;
}

static void setup(struct mGUIRunner* runner) {
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_CROSS, GBA_KEY_A);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_CIRCLE, GBA_KEY_B);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_OPTIONS, GBA_KEY_START);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_TOUCH_PAD, GBA_KEY_SELECT);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_UP, GBA_KEY_UP);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_DOWN, GBA_KEY_DOWN);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_LEFT, GBA_KEY_LEFT);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_RIGHT, GBA_KEY_RIGHT);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_L1, GBA_KEY_L);
	mapKey(&runner->core->inputMap, ORBIS_PAD_BUTTON_R1, GBA_KEY_R);

	unsigned mode;
	if (mCoreConfigGetUIntValue(&runner->config, "screenMode", &mode) &&
		mode <= M_PS4_SCREEN_STRETCH) {
		s_screenMode = (enum mPS4ScreenMode) mode;
	}
	if (mCoreConfigGetUIntValue(&runner->config, "filterMode", &mode)) {
		s_filterMode = mPS4FilterFromConfig((int) mode);
	}
	s_frameTexture = 0;
	uint32_t stride;
	uint32_t* pixels = mPS4RendererFramePixels(s_renderer, s_frameTexture, &stride);
	runner->core->setVideoBuffer(runner->core, pixels, stride);
	mRumbleIntegratorInit(&s_rumble);
	s_rumble.setRumble = setRumble;
	runner->core->setPeripheral(runner->core, mPERIPH_RUMBLE, &s_rumble.d);

	MutexInit(&s_audio.mutex);
	ConditionInit(&s_audio.cond);
	mAudioBufferInit(&s_audio.buffer, AUDIO_PACKET_COUNT * AUDIO_PACKET_FRAMES, 2);
	mAudioResamplerInit(&s_audio.resampler, mINTERPOLATOR_COSINE);
	mAudioResamplerSetDestination(&s_audio.resampler, &s_audio.buffer, 48000);
	unsigned sampleRate = runner->core->audioSampleRate(runner->core);
	if (!sampleRate) sampleRate = 32768;
	mAudioResamplerSetSource(&s_audio.resampler,
		runner->core->getAudioBuffer(runner->core), sampleRate, true);
	s_audio.initialized = true;
	runner->core->setAudioBufferSize(runner->core, AUDIO_PACKET_FRAMES);
	s_stream.postAudioBuffer = postAudioBuffer;
	s_stream.audioRateChanged = audioRateChanged;
	runner->core->setAVStream(runner->core, &s_stream);
}

static void teardown(struct mGUIRunner* runner) {
	UNUSED(runner);
	stopAudio();
	stopRumble();
}

static void gameLoaded(struct mGUIRunner* runner) {
	UNUSED(runner);
	clearAudioPackets();
	s_audio.running = true;
	if (ThreadCreate(&s_audio.thread, audioThread, NULL)) {
		s_audio.running = false;
	} else {
		s_audio.threadStarted = true;
	}
}

static void gameUnloaded(struct mGUIRunner* runner) {
	UNUSED(runner);
	stopAudio();
	stopRumble();
}

static void paused(struct mGUIRunner* runner) {
	UNUSED(runner);
	s_frameLimiter = true;
	clearAudioPackets();
	stopRumble();
}

static void unpaused(struct mGUIRunner* runner) {
	unsigned mode;
	if (mCoreConfigGetUIntValue(&runner->config, "screenMode", &mode) &&
		mode <= M_PS4_SCREEN_STRETCH) {
		s_screenMode = (enum mPS4ScreenMode) mode;
	}
	if (mCoreConfigGetUIntValue(&runner->config, "filterMode", &mode)) {
		s_filterMode = mPS4FilterFromConfig((int) mode);
	}
	mRumbleIntegratorReset(&s_rumble);
	clearAudioPackets();
}

static void prepareForFrame(struct mGUIRunner* runner) {
	s_frameTexture = (s_frameTexture + 1) % M_PS4_FRAME_TEXTURE_COUNT;
	uint32_t stride;
	uint32_t* pixels = mPS4RendererFramePixels(s_renderer, s_frameTexture, &stride);
	runner->core->setVideoBuffer(runner->core, pixels, stride);
}

static void drawTexture(unsigned texture, unsigned width, unsigned height, bool faded) {
	struct mPS4Rect destination = mPS4OutputRect(width, height,
		M_PS4_LOGICAL_WIDTH, M_PS4_LOGICAL_HEIGHT, s_screenMode);
	struct mPS4UvRect source = {
		0.0f, 0.0f,
		(float) width / M_PS4_FRAME_TEXTURE_WIDTH,
		(float) height / M_PS4_FRAME_TEXTURE_HEIGHT,
	};
	float level = faded ? 0.5f : 1.0f;
	struct mPS4Color color = { level, level, level, 1.0f };
	if (!mPS4RendererAddQuad(s_renderer, texture, destination, source, color, s_filterMode)) {
		s_running = false;
	}
}

static void drawFrame(struct mGUIRunner* runner, bool faded) {
	unsigned width;
	unsigned height;
	runner->core->currentVideoSize(runner->core, &width, &height);
	drawTexture(s_frameTexture, width, height, faded);
}

static void drawScreenshot(struct mGUIRunner* runner, const mColor* pixels,
	unsigned width, unsigned height, bool faded) {
	UNUSED(runner);
	if (width > M_PS4_FRAME_TEXTURE_WIDTH || height > M_PS4_FRAME_TEXTURE_HEIGHT) return;
	uint32_t stride;
	uint32_t* destination = mPS4RendererFramePixels(s_renderer, s_frameTexture, &stride);
	for (unsigned y = 0; y < height; ++y) {
		memcpy(destination + y * stride, pixels + y * width, width * sizeof(*pixels));
	}
	drawTexture(s_frameTexture, width, height, faded);
}

static void incrementScreenMode(struct mGUIRunner* runner) {
	s_screenMode = (s_screenMode + 1) % (M_PS4_SCREEN_STRETCH + 1);
	mCoreConfigSetUIntValue(&runner->config, "screenMode", s_screenMode);
}

static void setFrameLimiter(struct mGUIRunner* runner, bool limit) {
	UNUSED(runner);
	if (limit != s_frameLimiter) {
		s_unthrottledFrames = 0;
		s_lastFrameTime = 0;
		s_pacerSampleStart = 0;
		s_pacerSampleFrames = 0;
	}
	s_frameLimiter = limit;
}

static uint16_t pollGameInput(struct mGUIRunner* runner) {
	return (uint16_t) readPad(&runner->core->inputMap);
}

static bool running(struct mGUIRunner* runner) {
	struct mPS4SystemServiceEvent event;
	while (sceSystemServiceReceiveEvent(&event) == 0) {
		if (mPS4LifecycleActionFromEvent(event.eventType) == M_PS4_LIFECYCLE_RESUME) {
			clearAudioPackets();
			stopRumble();
			startupLog("system resume event received; audio queue cleared", "a");
		}
	}
	UNUSED(runner);
	return s_running;
}

static bool makeDirectory(const char* path) {
	return mkdir(path, 0777) == 0 || errno == EEXIST;
}

static bool prepareWritableTree(void) {
	static const char* const paths[] = {
		"/data/mgba", "/data/mgba/roms", "/data/mgba/saves",
		"/data/mgba/states", "/data/mgba/screenshots", "/data/mgba/cheats",
	};
	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
		if (!makeDirectory(paths[i])) return false;
	}
	return true;
}

static void startupLog(const char* message, const char* mode) {
	UNUSED(message);
	UNUSED(mode);
}

int main(void) {
	if (!prepareWritableTree()) return 1;
	startupLog("mGBA PS4 frontend starting", "w");
	startupLog("mgba-ps4: build marker native_pacer_telemetry_v16", "a");
	startupLog("main: before renderer create", "a");
	if (!mPS4RendererCreate(&s_renderer)) {
		startupLog("OpenGNM/VideoOut initialization failed", "a");
		return 2;
	}
	startupLog("main: after renderer create", "a");

	startupLog("main: before UserService/Pad/Audio init", "a");
	if (sceUserServiceGetInitialUser(&s_user) < 0 || scePadInit() < 0 || sceAudioOutInit() < 0) {
		startupLog("UserService/Pad initialization failed", "a");
		goto fail;
	}
	startupLog("main: after UserService/Pad/Audio init", "a");
	startupLog("main: before DualShock 4 open", "a");
	s_pad = scePadOpen(s_user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
	if (s_pad < 0) {
		startupLog("DualShock 4 open failed", "a");
		goto fail;
	}
	startupLog("main: after DualShock 4 open", "a");

	mPS4GUIFontSetRenderer(s_renderer);
	startupLog("main: before font atlas init", "a");
	struct GUIFont* font = GUIFontCreate();
	if (!font) {
		startupLog("font atlas initialization failed", "a");
		goto fail;
	}
	startupLog("main: after font atlas init", "a");

	static const char* const keyNames[] = {
		"Share", "L3", "R3", "Options", "Up", "Right", "Down", "Left",
		"L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square",
		"Reserved 16", "Reserved 17", "Reserved 18", "Reserved 19", "Touchpad",
	};
	struct mGUIRunner runner = {
		.params = {
			M_PS4_LOGICAL_WIDTH, M_PS4_LOGICAL_HEIGHT, font, "/data/mgba/roms",
			drawStart, drawEnd, pollInput, pollCursor, batteryState, NULL, NULL, NULL,
		},
		.keySources = (struct GUIInputKeys[]) {
			{ .name = "DualShock 4", .id = PS4_INPUT, .keyNames = keyNames,
				.nKeys = sizeof(keyNames) / sizeof(keyNames[0]) },
			{ .id = 0 },
		},
		.configExtra = (struct GUIMenuItem[]) {
			{ .title = "Screen mode", .data = GUI_V_S("screenMode"),
				.state = M_PS4_SCREEN_ASPECT_FIT,
				.validStates = (const char*[]) { "Pixel-perfect", "Aspect fit", "Stretch" },
				.nStates = 3 },
			{ .title = "Filtering", .data = GUI_V_S("filterMode"),
				.state = M_PS4_FILTER_NEAREST,
				.validStates = (const char*[]) { "Nearest", "Bilinear" }, .nStates = 2 },
		},
		.nConfigExtra = 2,
		.setup = setup,
		.teardown = teardown,
		.gameLoaded = gameLoaded,
		.gameUnloaded = gameUnloaded,
		.prepareForFrame = prepareForFrame,
		.drawFrame = drawFrame,
		.drawScreenshot = drawScreenshot,
		.paused = paused,
		.unpaused = unpaused,
		.incrementScreenMode = incrementScreenMode,
		.setFrameLimiter = setFrameLimiter,
		.pollGameInput = pollGameInput,
		.running = running,
	};
	startupLog("main: before mGUI init", "a");
	mGUIInit(&runner, "ps4");
	startupLog("main: after mGUI init", "a");
	mCoreConfigSetValue(&runner.config, "savegamePath", "/data/mgba/saves");
	mCoreConfigSetValue(&runner.config, "savestatePath", "/data/mgba/states");
	mCoreConfigSetValue(&runner.config, "screenshotPath", "/data/mgba/screenshots");
	mCoreConfigSetValue(&runner.config, "cheatsPath", "/data/mgba/cheats");
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_CROSS, GUI_INPUT_SELECT);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_CIRCLE, GUI_INPUT_BACK);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_TRIANGLE, GUI_INPUT_CANCEL);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_UP, GUI_INPUT_UP);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_DOWN, GUI_INPUT_DOWN);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_LEFT, GUI_INPUT_LEFT);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_RIGHT, GUI_INPUT_RIGHT);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_SQUARE, mGUI_INPUT_SCREEN_MODE);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_L2, mGUI_INPUT_FAST_FORWARD_HELD);
	mapKey(&runner.params.keyMap, ORBIS_PAD_BUTTON_R2, mGUI_INPUT_SCREENSHOT);
	startupLog("main: before mGUI runloop", "a");
	mGUIRunloop(&runner);
	startupLog("main: after mGUI runloop", "a");
	mGUIDeinit(&runner);
	GUIFontDestroy(font);
	stopRumble();
	scePadClose(s_pad);
	mPS4RendererDestroy(s_renderer);
	startupLog("mGBA PS4 frontend exited cleanly", "a");
	return 0;

fail:
	if (s_pad >= 0) scePadClose(s_pad);
	mPS4RendererDestroy(s_renderer);
	return 3;
}
