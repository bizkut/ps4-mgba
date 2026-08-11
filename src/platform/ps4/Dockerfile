FROM ubuntu:22.04 AS tools

ARG DEBIAN_FRONTEND=noninteractive
ARG SHADERC_COMMIT=d5f08ae5c5a9a45165578445cbd0f9adf0223448
ARG PSBC_COMMIT=a92a1228ea3a64e4be9f0e61c2a65a5aa7ffed92
ARG OPENGNM_COMMIT=4b295ca54c82c83acf308d1c646a2dfa9ae57350
ARG VULKAN_HEADERS_COMMIT=8c2683390a4b10c0483e3b531555dfb841e2cd84
ARG SPIRV_HEADERS_COMMIT=942fe4b988359a0750b79f0ae7ed735994d3147d

RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates git make ninja-build python3 python3-pip \
	build-essential libgl-dev libgles-dev libx11-dev libwayland-dev \
	libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
	&& rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir cmake mako pyyaml

WORKDIR /opt/build
RUN git clone https://github.com/google/shaderc.git && \
	cd shaderc && git checkout "${SHADERC_COMMIT}" && \
	./utils/git-sync-deps && \
	cmake -S . -B build -G Ninja -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release && \
	cmake --build build --target glslc_exe --parallel && install -m 0755 build/glslc/glslc /usr/local/bin/glslc
COPY src/platform/ps4/psbc-config.mak /tmp/psbc-config.mak
RUN git clone https://github.com/PS4-OpenGNM/opengnm-psbc.git psbc && \
	cd psbc && git checkout "${PSBC_COMMIT}" && cd .. && \
	git clone https://github.com/PS4-OpenGNM/opengnm.git OpenGNM && \
	cd OpenGNM && git checkout "${OPENGNM_COMMIT}" && cd .. && \
	git clone https://github.com/KhronosGroup/Vulkan-Headers.git Vulkan-Headers && \
	cd Vulkan-Headers && git checkout "${VULKAN_HEADERS_COMMIT}" && cd .. && \
	git clone https://github.com/KhronosGroup/SPIRV-Headers.git SPIRV-Headers && \
	cd SPIRV-Headers && git checkout "${SPIRV_HEADERS_COMMIT}" && cd .. && \
	mkdir -p opengnm-psbc/include && \
	cp /tmp/psbc-config.mak psbc/config.mak

RUN cd psbc && \
	python3 src/util/format/u_format_table.py --enums src/util/format/u_format.yaml > src/util/format/u_format_gen.h && \
	python3 src/util/format/u_format_table.py --header src/util/format/u_format.yaml > src/util/format/u_format_pack.h && \
	python3 src/util/format/u_format_table.py src/util/format/u_format.yaml > src/util/format/u_format_table.c && \
	python3 src/compiler/builtin_types_h.py src/compiler/builtin_types.h && \
	python3 src/compiler/builtin_types_c.py src/compiler/builtin_types.c && \
	python3 src/util/process_shader_stats.py src/util/shader_stats.rnc src/util/shader_stats.xml > src/util/shader_stats.h && \
	python3 src/vulkan/util/vk_struct_type_cast_gen.py --xml src/vulkan/registry/vk.xml --out src/vulkan/util/vk_struct_type_cast.h --beta false && \
	python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
		src/amd/packets/cp_pm4_table_data_gfx11.json src/amd/packets/pm4_it_opcodes_gfx11.h \
		src/amd/packets/cp_pm4_table_data_gfx12.json src/amd/packets/pm4_it_opcodes_gfx12.h \
		gfx11 packets_h > src/amd/common/amd_cp_packets_gfx11.h && \
	python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
		src/amd/packets/cp_pm4_table_data_gfx11.json src/amd/packets/pm4_it_opcodes_gfx11.h \
		src/amd/packets/cp_pm4_table_data_gfx12.json src/amd/packets/pm4_it_opcodes_gfx12.h \
		gfx12 packets_h > src/amd/common/amd_cp_packets_gfx12.h && \
	python3 src/amd/common/gfx10_format_table.py \
		src/util/format/u_format.yaml src/amd/registers/gfx10-rsrc.json src/amd/registers/gfx11-rsrc.json \
		> src/amd/common/gfx10_format_table.c && \
	python3 src/util/format_srgb.py > src/util/format_srgb.c && \
	make generated
RUN find psbc -name '*.py' -type f -exec chmod +x {} + && \
	make -C psbc -j"$(nproc)" PYTHON=python3 OPENGNM_INCLUDE=../OpenGNM/include VULKAN_INCLUDE=../Vulkan-Headers/include \
		BLAKE3_SRCS="src/util/blake3/blake3.c src/util/blake3/blake3_dispatch.c src/util/blake3/blake3_portable.c" && \
	install -m 0755 psbc/opengnm-psbc /usr/local/bin/psbc

FROM openorbisofficial/toolchain@sha256:f752d09f9d3227c729ee069f29a05450bd8fa4ef5e785c0929ab8479f7dc4e64

ARG OPENGNM_COMMIT=4b295ca54c82c83acf308d1c646a2dfa9ae57350
USER root
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates cmake git make ninja-build python3 \
	&& rm -rf /var/lib/apt/lists/*
COPY src/platform/ps4/create-fself /lib/OpenOrbisSDK/bin/linux/create-fself
RUN chmod 0755 /lib/OpenOrbisSDK/bin/linux/create-fself
COPY src/platform/ps4/create-gp4 /lib/OpenOrbisSDK/bin/linux/create-gp4
RUN chmod +x /lib/OpenOrbisSDK/bin/linux/create-gp4
COPY --from=tools /opt/build/shaderc /tmp/shaderc
RUN cd /tmp/shaderc && rm -rf build && \
	find . -name CMakeLists.txt -type f -exec sed -i 's/cmake_minimum_required(VERSION 3.22.1)/cmake_minimum_required(VERSION 3.16)/' {} + && \
	cmake -S . -B build -G Ninja -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release && \
	cmake --build build --target glslc_exe --parallel && \
	install -m 0755 build/glslc/glslc /usr/local/bin/glslc && \
	rm -rf /tmp/shaderc
COPY --from=tools /opt/build/psbc /tmp/psbc
COPY --from=tools /opt/build/OpenGNM /tmp/OpenGNM
COPY --from=tools /opt/build/Vulkan-Headers /tmp/Vulkan-Headers
COPY --from=tools /opt/build/SPIRV-Headers /tmp/SPIRV-Headers
RUN find /tmp/psbc -type f \( -name '*.o' -o -name 'opengnm-psbc' \) -delete && \
	printf '\nCXXFLAGS += -std=gnu++17\n' >> /tmp/psbc/config.mak && \
	make -C /tmp/psbc -j"$(nproc)" PYTHON=python3 OPENGNM_INCLUDE=../OpenGNM/include VULKAN_INCLUDE=../Vulkan-Headers/include \
		BLAKE3_SRCS="src/util/blake3/blake3.c src/util/blake3/blake3_dispatch.c src/util/blake3/blake3_portable.c" && \
	install -m 0755 /tmp/psbc/opengnm-psbc /usr/local/bin/psbc && \
	rm -rf /tmp/psbc /tmp/OpenGNM /tmp/Vulkan-Headers /tmp/SPIRV-Headers
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
