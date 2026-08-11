DESTDIR=/usr/local
BINDIR=/bin

OPENGNM_INCLUDE?=../OpenGNM/include
VULKAN_INCLUDE?=../Vulkan-Headers/include

SHARED_FLAGS=\
	-Iinclude/ \
	-Ilibpsbc/ \
	-I$(OPENGNM_INCLUDE) \
	-I$(VULKAN_INCLUDE) \
	-Isrc/ \
	-Isrc/amd \
	-Isrc/amd/common \
	-Isrc/amd/common/nir \
	-Isrc/amd/compiler \
	-Isrc/amd/vulkan \
	-Isrc/amd/vulkan/nir \
	-Isrc/compiler \
	-Isrc/compiler/nir \
	-Isrc/compiler/spirv \
	-Isrc/gallium/include \
	-Isrc/vulkan \
	-Isrc/vulkan/runtime \
	-Isrc/vulkan/runtime/bvh \
	-Isrc/vulkan/runtime/util \
	-Isrc/vulkan/util \
	-Isrc/mesa \
	-Isrc/mesa/main \
	-Isrc/util \
	-Icmd/psbc \
	-Iinclude/mesa \
	-D_XOPEN_SOURCE=500 \
	-DUTIL_ARCH_LITTLE_ENDIAN=1 \
	-DUTIL_ARCH_BIG_ENDIAN=0 \
	-DHAVE_STRUCT_TIMESPEC=1 \
	-DHAVE_PTHREAD=1 \
	-DHAVE_SYSCONF=1 \
	-DBLAKE3_NO_SSE2=1 \
	-DBLAKE3_NO_SSE41=1 \
	-DBLAKE3_NO_AVX2=1 \
	-DBLAKE3_NO_AVX512=1

ifeq ($(shell uname -s),Darwin)
SHARED_FLAGS += -D_DARWIN_C_SOURCE -DNO_FORMAT_ASM
endif
ifeq ($(shell uname -s),Linux)
SHARED_FLAGS += -D_GNU_SOURCE
endif

CC=cc
CXX=c++
LD=c++
AR=ar
PYTHON=python3
CFLAGS=-std=gnu11 -Wall -O2 -g $(SHARED_FLAGS)
CXXFLAGS=-std=gnu17 -Wall -O2 -g $(SHARED_FLAGS)
LDFLAGS=-lm -lpthread
