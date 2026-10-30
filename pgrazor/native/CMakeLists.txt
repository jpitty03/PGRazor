cmake_minimum_required(VERSION 3.22)
project(pgrazor_core LANGUAGES C CXX)

# -----------------------------------------------------------------------------
# Dobby (static, verified by native_build.py at
# commit 0932d69c320e786672361ab53825ba8f4245e9d3)
# is pulled in from -DDOBBY_SOURCE_DIR. Its bundled CMakeLists predates newer CMake
# minimum-version policy, so relax the floor for the whole configure.
# -----------------------------------------------------------------------------
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Compatibility floor for pinned Dobby" FORCE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # asm volatile() in the self-check needs GNU extensions

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Hidden visibility + PIC everywhere; the version script is the real export gate.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT DEFINED DOBBY_SOURCE_DIR)
  message(FATAL_ERROR
    "DOBBY_SOURCE_DIR is required: point it at the pinned Dobby source tree "
    "(commit 0932d69c320e786672361ab53825ba8f4245e9d3).")
endif()

# Static-only Dobby with logging, examples, tests and plugin symbols disabled.
# (Option names track the pinned Dobby; the coordinator verifies dobby_static is the
#  only Dobby artifact produced.)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)

add_subdirectory(${DOBBY_SOURCE_DIR} ${CMAKE_BINARY_DIR}/dobby EXCLUDE_FROM_ALL)

# Keep the verified vendor tree immutable. Its POSIX patcher treats the first
# byte AFTER a write as part of the write, changing an unrelated adjacent page
# to RX when a trampoline exactly fills its arena. That can revoke write access
# from an ART reference table on MuMu. Build the corrected source in our tree.
set(dobby_posix_patch "${DOBBY_SOURCE_DIR}/source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc")
file(READ "${dobby_posix_patch}" dobby_patch_text)
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

add_library(pgrazor_core SHARED
  pgrazor_native.cpp
  hook_engine.cpp
  throws.cpp
  adventure_sync.cpp
  catch_presentation.cpp
  transfer_policy.cpp
  ar_throw.cpp
  always_hit.cpp
  rocket_minigame.cpp
  catch_animation.cpp
  spin_camera.cpp
  incubation.cpp
  quick_spin.cpp
  buddy_feeding.cpp
  map_tap.cpp
  items.cpp
  accounts.cpp
  iv_display.cpp
  incoming_gifts.cpp
  encounter_berries.cpp
  battles.cpp
  rocket_actions.cpp
  rocket_combat.cpp
  outgoing_gifts.cpp
  transfer_execution.cpp
  transfer_facts.cpp
  catch_completion.cpp
  summary_completion.cpp
  details_completion.cpp
  details_recovery.cpp
  catch_canvas.cpp
  catch_discovery.cpp
  camera_sampler.cpp
  map_poi.cpp
  routes_gate.cpp
  routes_actions.cpp
  routes_discovery.cpp
  routes_services.cpp
  routes_observers.cpp
  routes_tracking.cpp
  shared_catch.cpp
  runtime_transport.cpp
  runtime_transport_jni.cpp
  runtime_bootstrap.cpp
  quickball_metadata.cpp
  quickball_host.cpp
  shared_metadata.cpp
  shared_host.cpp
  spin_host.cpp
  inventory_host.cpp
  runtime_host.cpp
  encounter_host.cpp
  interaction_host.cpp
  spawn_host.cpp
  combat_host.cpp
  camera_host.cpp
  account_host.cpp
  transfer_observers.cpp
  catch_wait.cpp
  camera_metadata.cpp
  routes_metadata.cpp
  routes_host.cpp
  catch_observers.cpp
  runtime_metadata.cpp
  unity_scheduler.cpp
  vgp_actions.cpp
  vgp_policy.cpp
  vgp_marker.cpp
  vgp_candidates.cpp
  vgp_results.cpp
  vgp_discovery.cpp
  vgp_maintenance.cpp
  vgp_aux.cpp
  vgp_controller.cpp
  smart_catch.cpp
  smart_catch_binding.cpp
  jni_core.cpp)

target_sources(pgrazor_core PRIVATE events/event_collector.cpp)
set_source_files_properties(events/event_collector.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

target_include_directories(pgrazor_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${DOBBY_SOURCE_DIR}/include)

target_link_libraries(pgrazor_core PRIVATE dobby_static)

# These observers hold pinned arguments across the game's original callback.
# Unwind their native roots if the original propagates an IL2CPP exception.
set_source_files_properties(transfer_observers.cpp unity_scheduler.cpp catch_wait.cpp shared_host.cpp catch_canvas.cpp inventory_host.cpp encounter_host.cpp combat_host.cpp camera_host.cpp interaction_host.cpp catch_observers.cpp spin_camera.cpp quick_spin.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
target_compile_options(pgrazor_core PRIVATE
  -fvisibility=hidden
  -fvisibility-inlines-hidden
  -fno-rtti
  -fno-exceptions
  -Wall -Wextra)

set(PGRAZOR_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/pgrazor.map")
target_link_options(pgrazor_core PRIVATE
  "-Wl,--version-script=${PGRAZOR_VERSION_SCRIPT}" # export only JNI_OnLoad + pgrazor_native_api
  "-Wl,--exclude-libs,ALL"                         # keep Dobby's static symbols unexported
  "-Wl,-z,defs"                                    # no undefined symbols at link time
  "-Wl,--gc-sections")
set_target_properties(pgrazor_core PROPERTIES LINK_DEPENDS "${PGRAZOR_VERSION_SCRIPT}")

# Android system log lib (optional; present under the NDK toolchain).
find_library(ANDROID_LOG_LIB log)
if(ANDROID_LOG_LIB)
  target_link_libraries(pgrazor_core PRIVATE ${ANDROID_LOG_LIB})
endif()

set_target_properties(pgrazor_core PROPERTIES OUTPUT_NAME "pgrazor-core")
