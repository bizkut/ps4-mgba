set(OPENGNM_ROOT "${PROJECT_SOURCE_DIR}/../../opengnm" CACHE PATH "OpenGNM source directory")
set(GLSLC_EXECUTABLE "glslc" CACHE FILEPATH "GLSL to SPIR-V compiler")
set(OPENGNM_PSBC_EXECUTABLE "opengnm-psbc" CACHE FILEPATH "SPIR-V to PS4 shader compiler")
set(CREATE_FSELF_EXECUTABLE "" CACHE FILEPATH "OpenOrbis create-fself tool")
set(CREATE_GP4_EXECUTABLE "" CACHE FILEPATH "OpenOrbis create-gp4 tool")
set(PKGTOOL_EXECUTABLE "" CACHE FILEPATH "OpenOrbis PkgTool.Core tool")

if(BUILD_PS4_TESTS AND NOT PS4)
	enable_testing()
	add_executable(mgba-ps4-platform-test platform.c platform-test.c)
	target_include_directories(mgba-ps4-platform-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	add_test(NAME ps4-platform COMMAND mgba-ps4-platform-test)
	if(EXISTS "${OPENGNM_ROOT}/CMakeLists.txt")
		set(OPENGNM_PLATFORM generic CACHE STRING "" FORCE)
		set(OPENGNM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
		add_subdirectory("${OPENGNM_ROOT}" "${CMAKE_BINARY_DIR}/opengnm" EXCLUDE_FROM_ALL)
		add_executable(mgba-ps4-renderer-command-test renderer-command-test.c)
		target_link_libraries(mgba-ps4-renderer-command-test PRIVATE opengnm::opengnm ${M_LIBRARY})
		add_test(NAME ps4-renderer-command COMMAND mgba-ps4-renderer-command-test)
	endif()
endif()

if(NOT PS4)
	return()
endif()

if(NOT OO_PS4_TOOLCHAIN)
	message(FATAL_ERROR "OO_PS4_TOOLCHAIN is required for PS4 builds")
endif()

if(NOT EXISTS "${OPENGNM_ROOT}/CMakeLists.txt")
	message(FATAL_ERROR "OPENGNM_ROOT does not contain OpenGNM: ${OPENGNM_ROOT}")
endif()
if(NOT IS_ABSOLUTE "${GLSLC_EXECUTABLE}")
	if(EXISTS "${PROJECT_SOURCE_DIR}/${GLSLC_EXECUTABLE}")
		get_filename_component(PS4_GLSLC "${PROJECT_SOURCE_DIR}/${GLSLC_EXECUTABLE}" ABSOLUTE)
	else()
		find_program(PS4_GLSLC NAMES "${GLSLC_EXECUTABLE}")
	endif()
	if(NOT PS4_GLSLC)
		message(FATAL_ERROR "GLSLC_EXECUTABLE was not found: ${GLSLC_EXECUTABLE}")
	endif()
	set(GLSLC_EXECUTABLE "${PS4_GLSLC}")
elseif(NOT EXISTS "${GLSLC_EXECUTABLE}")
	message(FATAL_ERROR "GLSLC_EXECUTABLE does not exist: ${GLSLC_EXECUTABLE}")
endif()
if(NOT IS_ABSOLUTE "${OPENGNM_PSBC_EXECUTABLE}")
	if(EXISTS "${PROJECT_SOURCE_DIR}/${OPENGNM_PSBC_EXECUTABLE}")
		get_filename_component(PS4_OPENGNM_PSBC
			"${PROJECT_SOURCE_DIR}/${OPENGNM_PSBC_EXECUTABLE}" ABSOLUTE)
	else()
		find_program(PS4_OPENGNM_PSBC NAMES "${OPENGNM_PSBC_EXECUTABLE}")
	endif()
	if(NOT PS4_OPENGNM_PSBC)
		message(FATAL_ERROR "OPENGNM_PSBC_EXECUTABLE was not found: ${OPENGNM_PSBC_EXECUTABLE}")
	endif()
	set(OPENGNM_PSBC_EXECUTABLE "${PS4_OPENGNM_PSBC}")
elseif(NOT EXISTS "${OPENGNM_PSBC_EXECUTABLE}")
	message(FATAL_ERROR "OPENGNM_PSBC_EXECUTABLE does not exist: ${OPENGNM_PSBC_EXECUTABLE}")
endif()
set(OPENGNM_PLATFORM orbis CACHE STRING "" FORCE)
set(OPENGNM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPENGNM_ROOT}" "${CMAKE_BINARY_DIR}/opengnm" EXCLUDE_FROM_ALL)

add_library(mgba-ps4-platform STATIC platform.c)
target_include_directories(mgba-ps4-platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(PS4_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/ps4-shaders")
file(MAKE_DIRECTORY "${PS4_SHADER_OUTPUT_DIR}")
function(ps4_compile_shader name stage)
	set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.glsl")
	set(spirv "${PS4_SHADER_OUTPUT_DIR}/${name}.spv")
	set(binary "${PS4_SHADER_OUTPUT_DIR}/${name}.sb")
	add_custom_command(
		OUTPUT "${spirv}"
		COMMAND "${GLSLC_EXECUTABLE}" -fshader-stage=${stage} "${source}" -o "${spirv}"
		DEPENDS "${source}"
		COMMENT "Compiling ${name} to SPIR-V"
		VERBATIM)
	add_custom_command(
		OUTPUT "${binary}"
		COMMAND "${OPENGNM_PSBC_EXECUTABLE}" -s ${stage} -f "${spirv}" -o "${binary}" -4
		DEPENDS "${spirv}"
		COMMENT "Compiling ${name} for PS4 base GFX7"
		VERBATIM)
	set_property(GLOBAL APPEND PROPERTY MGBA_PS4_SHADER_BINARIES "${binary}")
endfunction()

ps4_compile_shader(quad.vert vertex)
ps4_compile_shader(quad.frag fragment)
get_property(PS4_SHADER_BINARIES GLOBAL PROPERTY MGBA_PS4_SHADER_BINARIES)
add_custom_target(mgba-ps4-shaders DEPENDS ${PS4_SHADER_BINARIES})

add_library(ps4-compat STATIC ps4_compat.c)

add_executable(${BINARY_NAME}-ps4.elf ${GUI_SRC} gui-font.c renderer.c main.c)
add_dependencies(${BINARY_NAME}-ps4.elf mgba-ps4-shaders zlibstatic png_static)
target_link_libraries(${BINARY_NAME}-ps4.elf PRIVATE
	${BINARY_NAME} mgba-ps4-platform opengnm::opengnm png_static zlibstatic c c_setjmp kernel
	SceGnmDriver SceVideoOut SceUserService ScePad SceAudioOut SceSystemService ps4-compat)
set_target_properties(${BINARY_NAME}-ps4.elf PROPERTIES
	COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

install(TARGETS ${BINARY_NAME}-ps4.elf DESTINATION . COMPONENT ${BINARY_NAME}-dbg)
