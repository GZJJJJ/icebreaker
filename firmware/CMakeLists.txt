cmake_minimum_required(VERSION 3.16)

# ESP32 is a cross-compiled target without a standalone host-style crt0.
# Let ESP-IDF perform the real firmware link instead of linking CMake's
# compiler probe as an executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# The offline tool bundle's C++ frontend is unusable on this host. This
# project intentionally contains C-only application code; ESP-IDF still
# enables CXX globally, so skip its compiler probe.
set(CMAKE_CXX_COMPILER_WORKS TRUE CACHE INTERNAL "" FORCE)
set(CMAKE_CXX_COMPILER_FORCED TRUE CACHE INTERNAL "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(icebreaker_firmware LANGUAGES C ASM)
