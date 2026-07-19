FROM ubuntu:20.04 AS tools

ARG DEBIAN_FRONTEND=noninteractive
ARG SHADERC_COMMIT=d5f08ae5c5a9a45165578445cbd0f9adf0223448
ARG PSBC_COMMIT=e21e69960f387121428521988237744f6111f13f
ARG OPENGNM_COMMIT=e442acdb6de57962fa336f09c10d34cac0df8a71
ARG VULKAN_HEADERS_COMMIT=8c2683390a4b10c0483e3b531555dfb841e2cd84

RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates git make ninja-build python3 python3-pip \
	build-essential libgl-dev libgles-dev libx11-dev libwayland-dev \
	libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
	&& rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir cmake mako

WORKDIR /opt/build
COPY src/platform/ps4/psbc-opengnm.patch /tmp/psbc-opengnm.patch
RUN git clone https://github.com/google/shaderc.git && \
	cd shaderc && git checkout "${SHADERC_COMMIT}" && \
	./utils/git-sync-deps && \
	cmake -S . -B build -G Ninja -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release && \
	cmake --build build --target glslc_exe --parallel && install -m 0755 build/glslc/glslc /usr/local/bin/glslc
RUN git clone https://github.com/bizkut/psbc.git && \
	cd psbc && git checkout "${PSBC_COMMIT}" && \
	git apply /tmp/psbc-opengnm.patch && cd .. && \
	git clone https://github.com/bizkut/OpenGNM.git OpenGNM && \
	cd OpenGNM && git checkout "${OPENGNM_COMMIT}" && cd .. && \
	git clone https://github.com/KhronosGroup/Vulkan-Headers.git Vulkan-Headers && \
	cd Vulkan-Headers && git checkout "${VULKAN_HEADERS_COMMIT}" && cd .. && \
	mkdir -p opengnm-psbc/include && \
	make -C psbc -j"$(nproc)" GNM_INCLUDE=../OpenGNM/include && install -m 0755 psbc/psbc /usr/local/bin/psbc

FROM openorbisofficial/toolchain@sha256:f752d09f9d3227c729ee069f29a05450bd8fa4ef5e785c0929ab8479f7dc4e64

ARG OPENGNM_COMMIT=e442acdb6de57962fa336f09c10d34cac0df8a71
USER root
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates cmake git make ninja-build \
	&& rm -rf /var/lib/apt/lists/*
COPY --from=tools /usr/local/bin/glslc /usr/local/bin/glslc
COPY --from=tools /usr/local/bin/psbc /usr/local/bin/psbc
RUN git clone https://github.com/bizkut/OpenGNM.git /opt/opengnm && \
	cd /opt/opengnm && git checkout "${OPENGNM_COMMIT}"
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
