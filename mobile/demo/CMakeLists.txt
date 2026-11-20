# Built by the Android Gradle Plugin, or by build_apk.sh with the same NDK.
cmake_minimum_required(VERSION 3.18)
project(mg_demo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ../mg.c is the whole implementation: collector + Vulkan backend in one file.
add_library(mobile SHARED
        mobile_main.c
        eco.c
        hud.c
        ../mg.c
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(mobile PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${ANDROID_NDK}/sources/android/native_app_glue)

target_compile_options(mobile PRIVATE
        -Wall -Wextra -Werror
        -fno-exceptions -fno-unwind-tables
        -O2)

# m: cosf/sinf/sqrtf live in libm.so on Android, not libc.so. Leaving it out
# produces a .so that links and then fails to dlopen on the device.
# --no-undefined: turn that class of mistake into a build error, at build time.
target_link_libraries(mobile PRIVATE m android log vulkan)
target_link_options(mobile PRIVATE -Wl,--no-undefined)

# the glue needs the entry point kept
set_target_properties(mobile PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
