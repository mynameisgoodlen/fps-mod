#pragma once

#include <atomic>
#include <memory>

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace bedrock_sodium {

class BedrockSodiumMod {
public:
    static BedrockSodiumMod &instance();

    BedrockSodiumMod();
    BedrockSodiumMod(const BedrockSodiumMod &) = delete;
    BedrockSodiumMod &operator=(const BedrockSodiumMod &) = delete;

    bool load();
    bool enable();
    bool disable();
    bool unload();

    void onPresent();

private:
    bool installHooks();
    void removeHooks();
    void closeGraphicsLibraries();
    void refreshOverlay();

    ll::mod::NativeMod &mSelf;
    void *mEglLibrary{};
    void *mGlesLibrary{};

    pl::memory::HookHandle mSwapBuffersHook;
    pl::memory::HookHandle mSwapIntervalHook;
    pl::memory::HookHandle mMakeCurrentHook;
    pl::memory::HookHandle mDestroyContextHook;
    pl::memory::HookHandle mUseProgramHook;
    pl::memory::HookHandle mDeleteProgramHook;
    pl::memory::HookHandle mLinkProgramHook;

    std::atomic<bool> mModuleRegistered{false};
};

} // namespace bedrock_sodium
