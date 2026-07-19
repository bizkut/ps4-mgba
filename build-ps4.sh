#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_DIR="$SCRIPT_DIR"
BUILD_DIR="${PS4_BUILD_DIR:-$SCRIPT_DIR/build-ps4}"
STAGE_DIR="$BUILD_DIR/stage"
TITLE="${PS4_TITLE:-mGBA}"
VERSION="${PS4_VERSION:-1.00}"
TITLE_ID="${PS4_TITLE_ID:-MGBA00001}"
CONTENT_ID="${PS4_CONTENT_ID:-IV0000-MGBA00001_00-MGBAPS4PORT00000}"
TARGET="${PS4_TARGET:-mgba-ps4.elf}"

usage() {
	cat <<'EOF'
Usage: ./build-ps4.sh <action>

Actions:
  elf         configure and build the native PS4 mGUI frontend ELF
  stage       create eboot.bin and assemble the package staging tree
  pkg         stage and build the installable PKG
  macos-pkg   build a PKG with local OpenOrbis and Homebrew LLVM 18
  docker-image build the pinned PS4 builder image
  docker-pkg  build a PKG in the pinned PS4 builder image
  clean       remove only the PS4 build directory
  help        show this message

Required: OO_PS4_TOOLCHAIN
Optional: OPENGNM_ROOT, GLSLC_EXECUTABLE, OPENGNM_PSBC_EXECUTABLE,
           CREATE_FSELF_EXECUTABLE, CREATE_GP4_EXECUTABLE, PKGTOOL_EXECUTABLE,
           PS4_DOCKER_IMAGE
EOF
}

require_sdk() {
	: "${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN must point to the OpenOrbis SDK}"
	if [[ ! -f "$OO_PS4_TOOLCHAIN/link.x" ]]; then
		echo "Invalid OO_PS4_TOOLCHAIN: missing $OO_PS4_TOOLCHAIN/link.x" >&2
		exit 1
	fi
}

configure() {
	require_sdk
	local opengnm_root="${OPENGNM_ROOT:-$WORKSPACE_DIR/opengnm}"
	cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
		-DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/src/platform/ps4/CMakeToolchain.cmake" \
		-DOO_PS4_TOOLCHAIN="$OO_PS4_TOOLCHAIN" \
		-DOPENGNM_ROOT="$opengnm_root" \
		-DGLSLC_EXECUTABLE="${GLSLC_EXECUTABLE:-glslc}" \
		-DOPENGNM_PSBC_EXECUTABLE="${OPENGNM_PSBC_EXECUTABLE:-opengnm-psbc}" \
		-DCMAKE_BUILD_TYPE=Release
}

build_elf() {
	configure
	cmake --build "$BUILD_DIR" --target "$TARGET" --parallel
}

host_tools_dir() {
	if [[ "$(uname -s)" == Darwin ]]; then
		printf '%s' macos
	else
		printf '%s' linux
	fi
}

resolve_tools() {
	local host_dir
	host_dir="$(host_tools_dir)"
	CREATE_FSELF_EXECUTABLE="${CREATE_FSELF_EXECUTABLE:-$OO_PS4_TOOLCHAIN/bin/$host_dir/create-fself}"
	if [[ "$host_dir" == macos && ! -x "$CREATE_FSELF_EXECUTABLE" ]]; then
		CREATE_FSELF_EXECUTABLE="$OO_PS4_TOOLCHAIN/bin/macos/create-fself-macos"
	fi
	CREATE_GP4_EXECUTABLE="${CREATE_GP4_EXECUTABLE:-$OO_PS4_TOOLCHAIN/bin/$host_dir/create-gp4}"
	PKGTOOL_EXECUTABLE="${PKGTOOL_EXECUTABLE:-$OO_PS4_TOOLCHAIN/bin/$host_dir/PkgTool.Core}"
	for tool in "$CREATE_FSELF_EXECUTABLE" "$CREATE_GP4_EXECUTABLE" "$PKGTOOL_EXECUTABLE"; do
		if [[ ! -x "$tool" ]]; then
			echo "Missing package helper: $tool" >&2
			exit 1
		fi
	done
}

runtime_module() {
	local name="$1"
	if [[ -f "$OO_PS4_TOOLCHAIN/bin/data/modules/$name" ]]; then
		printf '%s' "$OO_PS4_TOOLCHAIN/bin/data/modules/$name"
	else
		printf '%s' "$OO_PS4_TOOLCHAIN/src/modules/$name"
	fi
}

stage() {
	build_elf
	resolve_tools
	cmake -E remove_directory "$STAGE_DIR"
	cmake -E make_directory "$STAGE_DIR/sce_sys" "$STAGE_DIR/sce_module" \
		"$STAGE_DIR/assets" "$STAGE_DIR/assets/misc"
	"$CREATE_FSELF_EXECUTABLE" -in="$BUILD_DIR/$TARGET" -out="$BUILD_DIR/mgba.oelf" \
		-eboot="$STAGE_DIR/eboot.bin" --paid 0x3800000000000011
	cmake -E copy "$SCRIPT_DIR/res/mgba-512.png" "$STAGE_DIR/sce_sys/icon0.png"
	cmake -E copy "$SCRIPT_DIR/res/font-new.png" "$STAGE_DIR/assets/font-new.png"
	cmake -E copy "$BUILD_DIR/ps4-shaders/quad.vert.sb" "$STAGE_DIR/assets/misc/quad.vert.sb"
	cmake -E copy "$BUILD_DIR/ps4-shaders/quad.frag.sb" "$STAGE_DIR/assets/misc/quad.frag.sb"
	cmake -E copy "$(runtime_module libc.prx)" "$STAGE_DIR/sce_module/libc.prx"
	cmake -E copy "$(runtime_module libSceFios2.prx)" "$STAGE_DIR/sce_module/libSceFios2.prx"

	local sfo="$STAGE_DIR/sce_sys/param.sfo"
	"$PKGTOOL_EXECUTABLE" sfo_new "$sfo"
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" APP_TYPE --type Integer --maxsize 4 --value 1
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" ATTRIBUTE --type Integer --maxsize 4 --value 0
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" CATEGORY --type Utf8 --maxsize 4 --value gd
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" SYSTEM_VER --type Integer --maxsize 4 --value 0
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
	"$PKGTOOL_EXECUTABLE" sfo_setentry "$sfo" VERSION --type Utf8 --maxsize 8 --value "$VERSION"
	echo "Staged PS4 files in $STAGE_DIR"
}

build_pkg() {
	stage
	(
		cd "$STAGE_DIR"
		"$CREATE_GP4_EXECUTABLE" -out=pkg.gp4 -content-id="$CONTENT_ID" \
			-files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx assets/font-new.png assets/misc/quad.vert.sb assets/misc/quad.frag.sb"
		"$PKGTOOL_EXECUTABLE" pkg_build pkg.gp4 "$BUILD_DIR"
	)
	echo "Built $BUILD_DIR/$CONTENT_ID.pkg"
}

macos_pkg() {
	if [[ "$(uname -s)" != Darwin ]]; then
		echo "macos-pkg requires macOS" >&2
		exit 1
	fi
	local llvm_prefix="${LLVM18_PREFIX:-$(brew --prefix llvm@18)}"
	export PATH="$llvm_prefix/bin:$PATH"
	build_pkg
}

docker_image() {
	docker build --platform linux/amd64 -f "$SCRIPT_DIR/src/platform/ps4/Dockerfile" \
		-t "${PS4_DOCKER_IMAGE:-mgba/ps4:local}" "$SCRIPT_DIR"
}

docker_pkg() {
	docker run --rm --platform linux/amd64 \
		-v "$SCRIPT_DIR:/home/mgba/src" -w /home/mgba/src \
		"${PS4_DOCKER_IMAGE:-mgba/ps4:local}" ./build-ps4.sh pkg
}

case "${1:-help}" in
	elf) build_elf ;;
	stage) stage ;;
	pkg) build_pkg ;;
	macos-pkg) macos_pkg ;;
	docker-image) docker_image ;;
	docker-pkg) docker_pkg ;;

	clean) cmake -E remove_directory "$BUILD_DIR" ;;
	help|-h|--help) usage ;;
	*) usage >&2; exit 2 ;;
esac
