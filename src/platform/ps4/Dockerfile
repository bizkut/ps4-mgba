FROM ubuntu:20.04 AS tools

ARG DEBIAN_FRONTEND=noninteractive
ARG SHADERC_COMMIT=d5f08ae5c5a9a45165578445cbd0f9adf0223448
ARG PSBC_COMMIT=a92a1228ea3a64e4be9f0e61c2a65a5aa7ffed92
ARG OPENGNM_COMMIT=4b295ca54c82c83acf308d1c646a2dfa9ae57350
ARG VULKAN_HEADERS_COMMIT=8c2683390a4b10c0483e3b531555dfb841e2cd84

RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates git make ninja-build python3 python3-pip \
	build-essential libgl-dev libgles-dev libx11-dev libwayland-dev \
	libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
	&& rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir cmake mako

WORKDIR /opt/build
RUN git clone https://github.com/google/shaderc.git && \
	cd shaderc && git checkout "${SHADERC_COMMIT}" && \
	./utils/git-sync-deps && \
	cmake -S . -B build -G Ninja -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release && \
	cmake --build build --target glslc_exe --parallel && install -m 0755 build/glslc/glslc /usr/local/bin/glslc
RUN git clone https://github.com/PS4-OpenGNM/opengnm-psbc.git psbc && \
	cd psbc && git checkout "${PSBC_COMMIT}" && cd .. && \
	git clone https://github.com/PS4-OpenGNM/opengnm.git OpenGNM && \
	cd OpenGNM && git checkout "${OPENGNM_COMMIT}" && cd .. && \
	git clone https://github.com/KhronosGroup/Vulkan-Headers.git SPIRV-Headers && \
	cd SPIRV-Headers && git checkout "${VULKAN_HEADERS_COMMIT}" && cd .. && \
	mkdir -p opengnm-psbc/include && \
	cat > psbc/config.mak <<'EOF'
DESTDIR=/usr/local
BINDIR=/bin

OPENGNM_INCLUDE?=../OpenGNM/include
VULKAN_INCLUDE?=../SPIRV-Headers/include

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
	-DHAVE_PTHREAD=1

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
EOF

RUN make -C psbc -j"$(nproc)" OPENGNM_INCLUDE=../OpenGNM/include VULKAN_INCLUDE=../SPIRV-Headers/include && \
	install -m 0755 psbc/opengnm-psbc /usr/local/bin/psbc

FROM openorbisofficial/toolchain@sha256:f752d09f9d3227c729ee069f29a05450bd8fa4ef5e785c0929ab8479f7dc4e64

ARG OPENGNM_COMMIT=4b295ca54c82c83acf308d1c646a2dfa9ae57350
USER root
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates cmake git make ninja-build python3 \
	&& rm -rf /var/lib/apt/lists/*
RUN cat > /lib/OpenOrbisSDK/bin/linux/create-fself <<'EOF'
#!/bin/sh
set -e
in="" out="" eboot="" paid="0x3800000000000011"
while [ $# -gt 0 ]; do
	case "$1" in
		-in=*) in="${1#*=}"; shift ;;
		-out=*) out="${1#*=}"; shift ;;
		-eboot=*) eboot="${1#*=}"; shift ;;
		--paid) paid="$2"; shift 2 ;;
		--paid=*) paid="${1#*=}"; shift ;;
		*) shift ;;
	esac
done
if [ -n "$eboot" ]; then
	eboot_dir=$(dirname "$eboot")
	eboot_base=$(basename "$eboot")
	mkdir -p "$eboot_dir"
	(
		cd "$eboot_dir"
		/lib/OpenOrbisSDK/bin/linux/create-eboot -in "$in" -paid "$paid"
		if [ "$eboot_base" != "eboot.bin" ]; then
			mv eboot.bin "$eboot_base"
		fi
	)
fi
if [ -n "$out" ]; then
	cp "$in" "$out"
fi
EOF
RUN chmod +x /lib/OpenOrbisSDK/bin/linux/create-fself
COPY src/platform/ps4/create-gp4 /lib/OpenOrbisSDK/bin/linux/create-gp4
RUN chmod +x /lib/OpenOrbisSDK/bin/linux/create-gp4
COPY --from=tools /usr/local/bin/glslc /usr/local/bin/glslc
COPY --from=tools /usr/local/bin/psbc /usr/local/bin/psbc
COPY src/platform/ps4/opengnm-orbis-stubs.patch /tmp/opengnm-orbis-stubs.patch
RUN git clone https://github.com/PS4-OpenGNM/opengnm.git /opt/opengnm && \
	cd /opt/opengnm && git checkout "${OPENGNM_COMMIT}" && \
	git apply /tmp/opengnm-orbis-stubs.patch
RUN useradd --create-home --shell /bin/bash mgba && mkdir -p /home/mgba/src && \
	chown -R mgba:mgba /home/mgba /opt/opengnm
USER mgba
ENV OO_PS4_TOOLCHAIN=/lib/OpenOrbisSDK \
	OPENGNM_ROOT=/opt/opengnm \
	GLSLC_EXECUTABLE=/usr/local/bin/glslc \
	OPENGNM_PSBC_EXECUTABLE=/usr/local/bin/psbc \
	PS4_BUILD_DIR=/home/mgba/src/build-ps4
VOLUME ["/home/mgba/src"]
WORKDIR /home/mgba/src
CMD ["./build-ps4.sh", "pkg"]
