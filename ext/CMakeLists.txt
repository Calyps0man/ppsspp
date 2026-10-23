set(ARMIPS_REGEXP OFF CACHE BOOL "" FORCE)
set(ARMIPS_LIBRARY_ONLY ON CACHE BOOL "" FORCE)
if(WIN32)
	set(ARMIPS_USE_STD_FILESYSTEM ON CACHE BOOL "" FORCE)
endif()

if(NOT PS3)
	add_subdirectory(armips)
endif()

if(NOT USING_GLES2 AND NOT NO_GL_BUILD)
	add_subdirectory(glew)
endif()

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "let's not build binaries we don't need" FORCE)
set(SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS ON CACHE BOOL "let's not use exceptions" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "we don't need spvremapper" FORCE)

# not really needed since we currently bypass the official cmakelists...
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "we don't need zlib examples" FORCE)

# This is really a workaround for an NDK 20 compiler issue (PPSSPP issue #12105), but shouldn't hurt.
if(ANDROID)
set(ENABLE_HLSL OFF CACHE BOOL "let's not build HLSL support we don't need" FORCE)
endif()

# This works around a bug in cpu_features when ${CMAKE_BINARY_DIR} != ${CMAKE_CURRENT_BINRARY_DIR}.
# We also disable the list_cpu_features binary.
add_subdirectory(cmake/cpu_features EXCLUDE_FROM_ALL)

set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_INSTALL OFF)
if(PS3)
	# Generic systems are not selected by glslang's platform switch. PSL1GHT
	# supplies pthread TLS, once and recursive mutexes used by this source.
	add_library(OSDependent STATIC glslang/glslang/OSDependent/Unix/ossource.cpp)
endif()
add_subdirectory(glslang EXCLUDE_FROM_ALL)
add_subdirectory(snappy)
add_subdirectory(lua-build)
add_subdirectory(at3_standalone)
add_subdirectory(udis86)
add_subdirectory(SPIRV-Cross-build)
add_subdirectory(rcheevos-build)
add_subdirectory(gason)
if(NOT NO_GL_BUILD)
	add_subdirectory(vma)
endif()
add_subdirectory(cityhash)
add_subdirectory(lzma-sdk)
add_subdirectory(basis_universal)
add_subdirectory(pugixml)
add_subdirectory(libkirk)
add_subdirectory(sfmt19937)
add_subdirectory(xbrz)

add_library(xxhash STATIC
	xxhash.c
	xxhash.h
)

set(aemu_postoffice_client_obj
	aemu_postoffice/client/postoffice.c
	aemu_postoffice/client/postoffice_mem_stdc.c
	aemu_postoffice/client/mutex_impl_cpp.cpp
	aemu_postoffice/client/delay_impl_cpp.cpp
	aemu_postoffice/client/log_impl_ppsspp.cpp
)

if(WIN32)
	list(APPEND aemu_postoffice_client_obj
		aemu_postoffice/client/sock_impl_windows.c
	)
endif()

if(LINUX OR ANDROID OR MACOSX OR IOS OR BSD)
	list(APPEND aemu_postoffice_client_obj
		aemu_postoffice/client/sock_impl_linux.c
	)
endif()

if(PS3)
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	set(postoffice_ps3_dir "${CMAKE_CURRENT_BINARY_DIR}/postoffice-ps3/client")
	execute_process(COMMAND "${Python3_EXECUTABLE}"
		"${CMAKE_SOURCE_DIR}/PS3/prepare-postoffice.py"
		"${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/client" "${postoffice_ps3_dir}"
		RESULT_VARIABLE postoffice_prepare_result)
	if(NOT postoffice_prepare_result EQUAL 0)
		message(FATAL_ERROR "Failed to prepare pinned PS3 postoffice overlay")
	endif()
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/PS3/prepare-postoffice.py"
		"${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/client/postoffice.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/client/postoffice_mem.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/client/sock_impl.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/aemu_postoffice_packets.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/client/postoffice_mem_stdc.c")
	list(REMOVE_ITEM aemu_postoffice_client_obj
		aemu_postoffice/client/postoffice.c aemu_postoffice/client/postoffice_mem_stdc.c)
	list(APPEND aemu_postoffice_client_obj
		"${postoffice_ps3_dir}/postoffice.c" "${postoffice_ps3_dir}/postoffice_mem_stdc.c"
		"${CMAKE_SOURCE_DIR}/PS3/PostofficeSocket.c")
endif()

add_library(aemu_postoffice_client STATIC
	${aemu_postoffice_client_obj}
)

if(PS3)
	target_include_directories(aemu_postoffice_client PRIVATE "${postoffice_ps3_dir}")
endif()

set(FT_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_BZIP2 OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_PNG OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)
if(NOT LIBRETRO AND NOT USE_SYSTEM_FREETYPE)
    add_subdirectory(freetype freetype-build)
endif()

if(NOT LIBRETRO)
    add_subdirectory(imgui)
endif()
if(NOT HTTPS_NOT_AVAILABLE)
	add_subdirectory(naett-build)
endif()
if(USE_DISCORD AND NOT IOS AND NOT LIBRETRO)
	add_subdirectory(discord-rpc-build)
endif()

if(NOT USE_SYSTEM_LIBCHDR)
	add_subdirectory(libchdr-build)
endif()

if(BUILD_BUNDLED_FFMPEG)
	add_subdirectory(ffmpeg-build)
endif()

if(ANDROID)
    if (ARM64)
        add_subdirectory(libadrenotools)
    endif()
endif()
