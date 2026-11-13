cmake_minimum_required(VERSION 3.22)
project(bedrock_sodium LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT ANDROID)
    message(FATAL_ERROR "Bedrock Sodium must be built with the Android NDK toolchain.")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "This initial build is intentionally restricted to armeabi-v7a; got ${CMAKE_ANDROID_ARCH_ABI}.")
endif()

set(MOD_ID "bedrock_sodium")
set(MOD_NAME "Bedrock Sodium")
set(MOD_AUTHOR "Bedrock Sodium contributors")
set(MOD_VERSION "0.1.0")
set(MOD_LIBRARY_NAME "bedrock_sodium")
set(MOD_MINECRAFT_VERSIONS "[\"1.26.62.*\"]")

include(FetchContent)

FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2
    SOURCE_SUBDIR cmake/preloader-sdk
)
FetchContent_MakeAvailable(preloader_android)
FetchContent_GetProperties(preloader_android)

if(NOT TARGET preloader)
    add_subdirectory("${preloader_android_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

add_library(${MOD_LIBRARY_NAME} SHARED
    src/main.cpp
    src/mod/BedrockSodium.cpp
)

set_target_properties(${MOD_LIBRARY_NAME} PROPERTIES
    OUTPUT_NAME "${MOD_LIBRARY_NAME}"
)

target_include_directories(${MOD_LIBRARY_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${preloader_android_SOURCE_DIR}/include"
)

target_compile_options(${MOD_LIBRARY_NAME} PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden -fvisibility-inlines-hidden>
)

target_link_options(${MOD_LIBRARY_NAME} PRIVATE
    "-Wl,-z,max-page-size=16384"
)

find_library(ANDROID_LOG_LIB log REQUIRED)
find_library(ANDROID_EGL_LIB EGL REQUIRED)
find_library(ANDROID_GLES_LIB GLESv3 REQUIRED)
find_library(ANDROID_LIB android REQUIRED)

# The launcher has already loaded libpreloader.so before loading preload-native mods.
# Linking against the target keeps the public SDK symbols and ABI declarations exact.
target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE
    preloader
    ${ANDROID_LOG_LIB}
    ${ANDROID_EGL_LIB}
    ${ANDROID_GLES_LIB}
    ${ANDROID_LIB}
    ${CMAKE_DL_LIBS}
)

if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "The Android NDK CMAKE_STRIP tool was not configured.")
endif()

add_custom_command(TARGET ${MOD_LIBRARY_NAME} POST_BUILD
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
    COMMENT "Stripping ${MOD_LIBRARY_NAME}"
    VERBATIM
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/manifest.json"
    @ONLY
)

set(LEVI_PACKAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/package")
set(LEVI_PACKAGE_MOD_DIR "${LEVI_PACKAGE_ROOT}/${MOD_ID}")
set(LEVI_PACKAGE_FILE "${CMAKE_CURRENT_BINARY_DIR}/${MOD_ID}-${MOD_VERSION}-armeabi-v7a.levipack")

add_custom_target(levi_package
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${LEVI_PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEVI_PACKAGE_MOD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${MOD_LIBRARY_NAME}>" "${LEVI_PACKAGE_MOD_DIR}/lib${MOD_LIBRARY_NAME}.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" "${LEVI_PACKAGE_MOD_DIR}/manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${LEVI_PACKAGE_ROOT}" "${CMAKE_COMMAND}" -E tar "cf" "${LEVI_PACKAGE_FILE}" --format=zip "${MOD_ID}"
    DEPENDS ${MOD_LIBRARY_NAME}
    COMMENT "Packaging ${MOD_NAME} for LeviLauncher"
    VERBATIM
)
