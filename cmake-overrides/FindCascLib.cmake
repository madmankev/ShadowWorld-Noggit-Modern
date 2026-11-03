include(FetchContent)

# Current retail clients use the TVFS/VFS root format supported by upstream CascLib.
FetchContent_Declare(
  casclib
  GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
  GIT_TAG        2a280f5a231966dc5d1b534978dd9f9f04a374cd
)

set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(CASC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(casclib)

# Optional patch step shipped by the archive library. The pinned submodule does
# not always contain it, so only run it when it is actually present instead of
# aborting the whole configure.
set(_casc_prefer_local_high_res
    "${CMAKE_SOURCE_DIR}/src/external/blizzard-archive-library/cmake-overrides/PreferLocalHighResCasc.cmake")

if(EXISTS "${_casc_prefer_local_high_res}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DCASC_SOURCE_ROOT=${casclib_SOURCE_DIR}/src"
            -P "${_casc_prefer_local_high_res}"
    COMMAND_ERROR_IS_FATAL ANY
  )
else()
  message(STATUS "PreferLocalHighResCasc.cmake not found in blizzard-archive-library; using upstream CascLib defaults.")
endif()

add_library(CascLib ALIAS casc_static)
set(CASCLIB_INCLUDE_DIR "${casclib_SOURCE_DIR}/src")
set(CascLib_FOUND TRUE)
