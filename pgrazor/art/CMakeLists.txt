cmake_minimum_required(VERSION 3.22)
project(pgrazor_art LANGUAGES C CXX)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Pinned vendor compatibility" FORCE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
foreach(dep LSPLANT_SOURCE_DIR DOBBY_SOURCE_DIR XZ_SOURCE_DIR)
  if(NOT IS_DIRECTORY "${${dep}}")
    message(FATAL_ERROR "${dep} must name the verified offline source tree")
  endif()
endforeach()
set(LSPLANT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LSPLANT_STANDALONE OFF CACHE BOOL "" FORCE)
set(DEX_BUILDER_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
# Build-local LSPlant correction: publishing a native replacement and its backup
# must be one suspended-mutator operation. Init itself performs JNI work and must
# not be suspended wholesale. Keep the pinned dependency checkout immutable.
set(lsplant_original "${LSPLANT_SOURCE_DIR}/lsplant/src/main/jni")
file(TO_CMAKE_PATH "${lsplant_original}" lsplant_original)
set(lsplant_local "${CMAKE_BINARY_DIR}/lsplant-source")
file(COPY "${lsplant_original}/" DESTINATION "${lsplant_local}" PATTERN "external" EXCLUDE)
file(READ "${lsplant_local}/CMakeLists.txt" lsplant_cmake)
string(REPLACE "add_subdirectory(external/dex_builder)"
  "add_subdirectory(\"${lsplant_original}/external/dex_builder\" \"${CMAKE_BINARY_DIR}/dex_builder\")"
  lsplant_cmake "${lsplant_cmake}")
file(WRITE "${lsplant_local}/CMakeLists.txt" "${lsplant_cmake}")
set(helper_path "${lsplant_local}/include/utils/hook_helper.hpp")
file(READ "${helper_path}" helper_text)
set(old_hook [=[return hooker = hook(hooker.address_ = dlsym<Sym>(match_prefix),
                             reinterpret_cast<void *>(hooker.replace_));]=])
string(FIND "${helper_text}" "${old_hook}" hook_index)
if(hook_index EQUAL -1)
  message(FATAL_ERROR "Pinned LSPlant publication code changed; review correction")
endif()
string(REPLACE "namespace lsplant {" [=[extern "C" bool pgrazor_art_publication_begin() noexcept;
extern "C" void pgrazor_art_publication_end() noexcept;
struct PgrArtPublicationGuard {
    bool active = pgrazor_art_publication_begin();
    ~PgrArtPublicationGuard() { if (active) pgrazor_art_publication_end(); }
};
namespace lsplant {]=] helper_text "${helper_text}")
string(REPLACE "${old_hook}" [=[hooker.address_ = dlsym<Sym>(match_prefix);
        if (!hooker.address_) return false;
        PgrArtPublicationGuard guard;
        if (!guard.active) return false;
        return hooker = hook(hooker.address_, reinterpret_cast<void *>(hooker.replace_));]=]
  helper_text "${helper_text}")
string(REPLACE "if (hooker.address_ && info_.inline_unhooker && info_.inline_unhooker(hooker.address_)) {"
  [=[if (!hooker.address_ || !info_.inline_unhooker) return false;
        PgrArtPublicationGuard guard;
        if (!guard.active) return false;
        if (info_.inline_unhooker(hooker.address_)) {]=] helper_text "${helper_text}")
file(WRITE "${helper_path}" "${helper_text}")
add_subdirectory("${lsplant_local}" "${CMAKE_BINARY_DIR}/lsplant" EXCLUDE_FROM_ALL)
add_subdirectory("${DOBBY_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/dobby" EXCLUDE_FROM_ALL)
# Match the gameplay core's page-end correction without modifying pinned inputs.
# A write ending on a boundary must not revoke write access on the next ART page.
file(READ "${DOBBY_SOURCE_DIR}/source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc" dobby_patch_text)
set(dobby_patch_old "ALIGN_FLOOR((uintptr_t)address + buffer_size, page_size)")
string(FIND "${dobby_patch_text}" "${dobby_patch_old}" dobby_patch_index)
if(dobby_patch_index EQUAL -1)
  message(FATAL_ERROR "Pinned Dobby patch boundary changed; review local correction")
endif()
string(REPLACE "${dobby_patch_old}"
  "ALIGN_FLOOR((uintptr_t)address + (buffer_size ? buffer_size - 1 : 0), page_size)"
  dobby_patch_text "${dobby_patch_text}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dobby-code-patch-posix.cc" "${dobby_patch_text}")
get_target_property(dobby_static_sources dobby_static SOURCES)
list(FILTER dobby_static_sources EXCLUDE REGEX "code-patch-tool-posix\\.cc$")
set_property(TARGET dobby_static PROPERTY SOURCES "${dobby_static_sources}")
target_sources(dobby_static PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/dobby-code-patch-posix.cc")
set(XZ_LIB "${XZ_SOURCE_DIR}/linux/lib/xz")
add_library(pgrazor_xz STATIC
  "${XZ_LIB}/xz_crc32.c" "${XZ_LIB}/xz_crc64.c" "${XZ_LIB}/xz_sha256.c"
  "${XZ_LIB}/xz_dec_stream.c" "${XZ_LIB}/xz_dec_lzma2.c" "${XZ_LIB}/xz_dec_bcj.c")
target_include_directories(pgrazor_xz PUBLIC "${XZ_SOURCE_DIR}/userspace" "${XZ_SOURCE_DIR}/linux/include/linux")
target_compile_definitions(pgrazor_xz PUBLIC XZ_USE_CRC64 XZ_USE_SHA256 XZ_DEC_X86 XZ_DEC_ARM XZ_DEC_ARMTHUMB XZ_DEC_ARM64)
add_library(pgrazor_art SHARED art_symbols.cpp java_hooks.cpp)
set_target_properties(pgrazor_art PROPERTIES OUTPUT_NAME "pgrazor-art")
target_compile_options(pgrazor_art PRIVATE -Wall -Wextra -fexceptions -frtti)
target_link_libraries(pgrazor_art PRIVATE lsplant_static dobby_static pgrazor_xz log dl z)
target_link_options(pgrazor_art PRIVATE
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pgrazor-art.map"
  "-Wl,--exclude-libs,ALL" "-Wl,-z,defs" "-Wl,-Map,${CMAKE_BINARY_DIR}/pgrazor-art.link.map")
set_property(TARGET pgrazor_art APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pgrazor-art.map")
