#include "mod/BedrockSodium.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <string_view>

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

namespace bedrock_sodium {
namespace detail {

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);
using EglMakeCurrentFn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using EglDestroyContextFn = EGLBoolean (*)(EGLDisplay, EGLContext);
using GlUseProgramFn = void (*)(GLuint);
using GlDeleteProgramFn = void (*)(GLuint);
using GlLinkProgramFn = void (*)(GLuint);

constexpr std::string_view ModuleId = "bedrock_sodium.main";

std::atomic<BedrockSodiumMod *> gMod{nullptr};
std::atomic<bool> gModuleEnabled{true};
std::atomic<bool> gShowFps{true};
std::atomic<bool> gDisableVsync{false};
std::atomic<bool> gProgramBindCache{false};
std::atomic<bool> gProgramBindCacheReady{false};
std::atomic<std::uint64_t> gContextEpoch{1};
std::atomic<std::uint64_t> gSkippedProgramBinds{0};
std::atomic<float> gFps{0.0f};
std::atomic<float> gFrameMilliseconds{0.0f};

void *gOriginalSwapBuffers{};
void *gOriginalSwapInterval{};
void *gOriginalMakeCurrent{};
void *gOriginalDestroyContext{};
void *gOriginalUseProgram{};
void *gOriginalDeleteProgram{};
void *gOriginalLinkProgram{};

struct ProgramCache {
    EGLContext context{EGL_NO_CONTEXT};
    GLuint program{};
    std::uint64_t epoch{};
    bool valid{};
};

thread_local ProgramCache tProgramCache;
thread_local EGLContext tCurrentEglContext{EGL_NO_CONTEXT};

bool parseBool(std::string_view value) {
    return value == "true" || value == "1" || value == "on" || value == "yes";
}

template <typename Function>
Function functionFrom(void *address) {
    return reinterpret_cast<Function>(address);
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    const auto original = functionFrom<EglSwapBuffersFn>(gOriginalSwapBuffers);
    if (auto *mod = gMod.load(std::memory_order_acquire)) {
        mod->onPresent();
    }
    return original ? original(display, surface) : EGL_FALSE;
}

EGLBoolean swapIntervalDetour(EGLDisplay display, EGLint interval) {
    const auto original = functionFrom<EglSwapIntervalFn>(gOriginalSwapInterval);
    if (!original) {
        return EGL_FALSE;
    }
    if (gModuleEnabled.load(std::memory_order_relaxed) &&
        gDisableVsync.load(std::memory_order_relaxed)) {
        interval = 0;
    }
    return original(display, interval);
}

EGLBoolean makeCurrentDetour(EGLDisplay display, EGLSurface draw,
                             EGLSurface read, EGLContext context) {
    const auto original = functionFrom<EglMakeCurrentFn>(gOriginalMakeCurrent);
    if (!original) {
        return EGL_FALSE;
    }
    const EGLBoolean result = original(display, draw, read, context);
    if (result == EGL_TRUE) {
        if (tCurrentEglContext != context) {
            gContextEpoch.fetch_add(1, std::memory_order_relaxed);
            tProgramCache.valid = false;
            tCurrentEglContext = context;
        }
    }
    return result;
}

EGLBoolean destroyContextDetour(EGLDisplay display, EGLContext context) {
    const auto original = functionFrom<EglDestroyContextFn>(gOriginalDestroyContext);
    if (!original) {
        return EGL_FALSE;
    }
    const EGLBoolean result = original(display, context);
    if (result == EGL_TRUE) {
        gContextEpoch.fetch_add(1, std::memory_order_relaxed);
        if (tProgramCache.context == context) {
            tProgramCache.valid = false;
        }
        if (tCurrentEglContext == context) {
            tCurrentEglContext = EGL_NO_CONTEXT;
        }
    }
    return result;
}

void useProgramDetour(GLuint program) {
    const auto original = functionFrom<GlUseProgramFn>(gOriginalUseProgram);
    if (!original) {
        return;
    }

    if (!gModuleEnabled.load(std::memory_order_relaxed) ||
        !gProgramBindCache.load(std::memory_order_relaxed) ||
        !gProgramBindCacheReady.load(std::memory_order_relaxed)) {
        tProgramCache.valid = false;
        original(program);
        return;
    }

    const EGLContext context = eglGetCurrentContext();
    const std::uint64_t epoch = gContextEpoch.load(std::memory_order_relaxed);

    if (context != EGL_NO_CONTEXT && tProgramCache.valid &&
        tProgramCache.context == context && tProgramCache.epoch == epoch &&
        tProgramCache.program == program) {
        gSkippedProgramBinds.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    original(program);
    tProgramCache.context = context;
    tProgramCache.program = program;
    tProgramCache.epoch = epoch;
    tProgramCache.valid = context != EGL_NO_CONTEXT;
}

void deleteProgramDetour(GLuint program) {
    const auto original = functionFrom<GlDeleteProgramFn>(gOriginalDeleteProgram);
    if (original) {
        original(program);
    }
    if (tProgramCache.program == program) {
        tProgramCache.valid = false;
    }
}

void linkProgramDetour(GLuint program) {
    const auto original = functionFrom<GlLinkProgramFn>(gOriginalLinkProgram);
    if (original) {
        original(program);
    }
    if (tProgramCache.program == program) {
        tProgramCache.valid = false;
    }
}

void onModuleToggle(std::string_view, bool enabled) {
    gModuleEnabled.store(enabled, std::memory_order_relaxed);
}

void onConfigChanged(std::string_view, std::string_view key,
                     std::string_view value) {
    const bool enabled = parseBool(value);
    if (key == "show_fps") {
        gShowFps.store(enabled, std::memory_order_relaxed);
    } else if (key == "disable_vsync") {
        gDisableVsync.store(enabled, std::memory_order_relaxed);
    } else if (key == "program_bind_cache") {
        gProgramBindCache.store(enabled, std::memory_order_relaxed);
    }
}

} // namespace detail

BedrockSodiumMod &BedrockSodiumMod::instance() {
    static BedrockSodiumMod mod;
    return mod;
}

BedrockSodiumMod::BedrockSodiumMod()
    : mSelf(*ll::mod::NativeMod::current()) {}

bool BedrockSodiumMod::load() {
    mSelf.getLogger().info("Bedrock Sodium {} loaded for armeabi-v7a", "0.1.0");
    return true;
}

bool BedrockSodiumMod::enable() {
    detail::gMod.store(this, std::memory_order_release);
    detail::gModuleEnabled.store(true, std::memory_order_relaxed);

    const bool registered = pl::modmenu::ModuleBuilder(
        std::string(detail::ModuleId), "Bedrock Sodium")
        .modId(mSelf.getId())
        .description("Performance HUD and conservative OpenGL call optimizations")
        .defaultEnabled(true)
        .onToggle(detail::onModuleToggle)
        .onConfigChanged(detail::onConfigChanged)
        .config("show_fps", "FPS / frame-time HUD",
                pl::modmenu::ConfigType::Toggle, "true")
        .config("disable_vsync", "Disable VSync (may increase power use)",
                pl::modmenu::ConfigType::Toggle, "false")
        .config("program_bind_cache", "Skip redundant GL program binds (experimental)",
                pl::modmenu::ConfigType::Toggle, "false")
        .registerModule();

    mModuleRegistered.store(registered, std::memory_order_release);
    if (!registered) {
        mSelf.getLogger().error("Failed to register Bedrock Sodium Mod Menu module");
    }

    const bool hooksReady = installHooks();
    if (!hooksReady) {
        mSelf.getLogger().warn("Could not install the EGL present hook; FPS HUD will remain unavailable");
    }

    mSelf.getLogger().info("Bedrock Sodium enabled; EGL hooks: {}", hooksReady ? "ready" : "partial");
    return true;
}

bool BedrockSodiumMod::disable() {
    detail::gModuleEnabled.store(false, std::memory_order_relaxed);
    detail::gProgramBindCacheReady.store(false, std::memory_order_relaxed);
    detail::gMod.store(nullptr, std::memory_order_release);

    removeHooks();

    if (mModuleRegistered.load(std::memory_order_acquire)) {
        pl::modmenu::submitDrawCommands(detail::ModuleId, std::span<const pl::modmenu::DrawCommand>{});
        pl::modmenu::unregisterModule(detail::ModuleId);
        mModuleRegistered.store(false, std::memory_order_release);
    }

    return true;
}

bool BedrockSodiumMod::unload() {
    disable();
    closeGraphicsLibraries();
    mSelf.getLogger().info("Bedrock Sodium unloaded");
    return true;
}

bool BedrockSodiumMod::installHooks() {
    if (!mEglLibrary) {
        mEglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!mGlesLibrary) {
        mGlesLibrary = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!mEglLibrary || !mGlesLibrary) {
        const char *error = dlerror();
        mSelf.getLogger().error("Failed to open EGL/GLES libraries: {}",
                                error ? error : "unknown error");
        return false;
    }

    auto *swapBuffers = dlsym(mEglLibrary, "eglSwapBuffers");
    if (swapBuffers) {
        mSwapBuffersHook = pl::memory::HookHandle(
            swapBuffers, reinterpret_cast<void *>(&detail::swapBuffersDetour),
            &detail::gOriginalSwapBuffers);
    }

    auto *swapInterval = dlsym(mEglLibrary, "eglSwapInterval");
    if (swapInterval) {
        mSwapIntervalHook = pl::memory::HookHandle(
            swapInterval, reinterpret_cast<void *>(&detail::swapIntervalDetour),
            &detail::gOriginalSwapInterval);
    }

    auto *makeCurrent = dlsym(mEglLibrary, "eglMakeCurrent");
    if (makeCurrent) {
        mMakeCurrentHook = pl::memory::HookHandle(
            makeCurrent, reinterpret_cast<void *>(&detail::makeCurrentDetour),
            &detail::gOriginalMakeCurrent);
    }

    auto *destroyContext = dlsym(mEglLibrary, "eglDestroyContext");
    if (destroyContext) {
        mDestroyContextHook = pl::memory::HookHandle(
            destroyContext, reinterpret_cast<void *>(&detail::destroyContextDetour),
            &detail::gOriginalDestroyContext);
    }

    auto *useProgram = dlsym(mGlesLibrary, "glUseProgram");
    if (useProgram) {
        mUseProgramHook = pl::memory::HookHandle(
            useProgram, reinterpret_cast<void *>(&detail::useProgramDetour),
            &detail::gOriginalUseProgram);
    }

    auto *deleteProgram = dlsym(mGlesLibrary, "glDeleteProgram");
    if (deleteProgram) {
        mDeleteProgramHook = pl::memory::HookHandle(
            deleteProgram, reinterpret_cast<void *>(&detail::deleteProgramDetour),
            &detail::gOriginalDeleteProgram);
    }

    auto *linkProgram = dlsym(mGlesLibrary, "glLinkProgram");
    if (linkProgram) {
        mLinkProgramHook = pl::memory::HookHandle(
            linkProgram, reinterpret_cast<void *>(&detail::linkProgramDetour),
            &detail::gOriginalLinkProgram);
    }

    const bool cacheHooksReady = mUseProgramHook.installed() &&
        mDeleteProgramHook.installed() && mLinkProgramHook.installed() &&
        mMakeCurrentHook.installed() && mDestroyContextHook.installed();
    detail::gProgramBindCacheReady.store(cacheHooksReady, std::memory_order_relaxed);

    if (!cacheHooksReady) {
        mSelf.getLogger().warn("GL program-bind cache unavailable: required invalidation hooks did not all install");
    }

    return mSwapBuffersHook.installed();
}

void BedrockSodiumMod::removeHooks() {
    mSwapBuffersHook.reset();
    mSwapIntervalHook.reset();
    mMakeCurrentHook.reset();
    mDestroyContextHook.reset();
    mUseProgramHook.reset();
    mDeleteProgramHook.reset();
    mLinkProgramHook.reset();

    detail::gOriginalSwapBuffers = nullptr;
    detail::gOriginalSwapInterval = nullptr;
    detail::gOriginalMakeCurrent = nullptr;
    detail::gOriginalDestroyContext = nullptr;
    detail::gOriginalUseProgram = nullptr;
    detail::gOriginalDeleteProgram = nullptr;
    detail::gOriginalLinkProgram = nullptr;
}

void BedrockSodiumMod::closeGraphicsLibraries() {
    if (mGlesLibrary) {
        dlclose(mGlesLibrary);
        mGlesLibrary = nullptr;
    }
    if (mEglLibrary) {
        dlclose(mEglLibrary);
        mEglLibrary = nullptr;
    }
}

void BedrockSodiumMod::onPresent() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    static thread_local auto sampleStart = now;
    static thread_local auto lastOverlay = Clock::time_point{};
    static thread_local std::uint32_t frameCount = 0;
    ++frameCount;

    const auto sampleElapsed = now - sampleStart;
    if (sampleElapsed >= std::chrono::seconds(1)) {
        const double elapsedMs = std::chrono::duration<double, std::milli>(sampleElapsed).count();
        const float fps = elapsedMs > 0.0
            ? static_cast<float>(static_cast<double>(frameCount) * 1000.0 / elapsedMs)
            : 0.0f;
        detail::gFps.store(fps, std::memory_order_relaxed);
        detail::gFrameMilliseconds.store(fps > 0.0f ? 1000.0f / fps : 0.0f,
                                         std::memory_order_relaxed);
        sampleStart = now;
        frameCount = 0;
    }

    if (!mModuleRegistered.load(std::memory_order_acquire) ||
        !detail::gModuleEnabled.load(std::memory_order_relaxed) ||
        !detail::gShowFps.load(std::memory_order_relaxed)) {
        return;
    }

    if (lastOverlay.time_since_epoch().count() != 0 &&
        now - lastOverlay < std::chrono::milliseconds(250)) {
        return;
    }
    lastOverlay = now;
    refreshOverlay();
}

void BedrockSodiumMod::refreshOverlay() {
    const float fps = detail::gFps.load(std::memory_order_relaxed);
    const float frameMs = detail::gFrameMilliseconds.load(std::memory_order_relaxed);
    const std::uint64_t skipped = detail::gSkippedProgramBinds.load(std::memory_order_relaxed);

    char text[128]{};
    if (fps > 0.0f) {
        std::snprintf(text, sizeof(text), "Bedrock Sodium | %.0f FPS | %.2f ms | GL skips: %llu",
                      fps, frameMs, static_cast<unsigned long long>(skipped));
    } else {
        std::snprintf(text, sizeof(text), "Bedrock Sodium | measuring FPS...");
    }

    const std::array<pl::modmenu::DrawCommand, 1> commands{{
        pl::modmenu::DrawCommand{
            .type = pl::modmenu::DrawCommandType::Text,
            .x = 16.0f,
            .y = 24.0f,
            .color = 0xFFFFFFFFu,
            .size = 18.0f,
            .text = text,
        },
    }};
    pl::modmenu::submitDrawCommands(detail::ModuleId, commands);
}

} // namespace bedrock_sodium
