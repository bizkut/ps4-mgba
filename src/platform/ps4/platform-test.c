#include "platform.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		return 1; \
	} \
} while (0)

int main(void) {
	struct mPS4Rect rect = mPS4OutputRect(240, 160, 1920, 1080, M_PS4_SCREEN_PIXEL_PERFECT);
	CHECK(rect.x == 240 && rect.y == 60 && rect.width == 1440 && rect.height == 960);
	rect = mPS4OutputRect(240, 160, 1280, 720, M_PS4_SCREEN_ASPECT_FIT);
	CHECK(rect.x == 100 && rect.y == 0 && rect.width == 1080 && rect.height == 720);
	rect = mPS4OutputRect(160, 144, 1920, 1080, M_PS4_SCREEN_ASPECT_FIT);
	CHECK(rect.x == 360 && rect.y == 0 && rect.width == 1200 && rect.height == 1080);
	rect = mPS4OutputRect(240, 160, 1280, 720, M_PS4_SCREEN_STRETCH);
	CHECK(rect.x == 0 && rect.y == 0 && rect.width == 1280 && rect.height == 720);
	CHECK(mPS4FilterFromConfig(1) == M_PS4_FILTER_BILINEAR);
	CHECK(mPS4FilterFromConfig(99) == M_PS4_FILTER_NEAREST);
	CHECK(mPS4MapGamePad(M_PS4_PAD_CROSS | M_PS4_PAD_L1 | M_PS4_PAD_RIGHT) ==
		(M_PS4_KEY_A | M_PS4_KEY_L | M_PS4_KEY_RIGHT));
	char path[64];
	CHECK(mPS4WritablePath(path, sizeof(path), "screenshots"));
	CHECK(strcmp(path, "/data/mgba/screenshots") == 0);
	CHECK(!mPS4WritablePath(path, 8, "screenshots"));
	CHECK(!mPS4WritablePath(path, sizeof(path), "/absolute"));
	CHECK(mPS4LifecycleActionFromEvent(M_PS4_SYSTEM_EVENT_ON_RESUME) ==
		M_PS4_LIFECYCLE_RESUME);
	CHECK(mPS4LifecycleActionFromEvent(0x10000001) == M_PS4_LIFECYCLE_NONE);
	return 0;
}
