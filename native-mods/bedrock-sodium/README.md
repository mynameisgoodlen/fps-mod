# Bedrock Sodium — ARMv7 Preloader mod

An early, native Bedrock performance-mod foundation for LeviLauncher. This is **not** Java Edition Sodium and does not port LeviLamina.

## Target

- Android ABI: `armeabi-v7a` (32-bit ARM)
- Minecraft manifest match: `1.26.62.*`
- Loader: LeviLauncher Android Preloader SDK `0.2.2`
- Package type: `preload-native` (`.levipack`)

## Implemented in this first source revision

1. **FPS / frame-time HUD** — counts calls to `eglSwapBuffers` and submits a small Mod Menu overlay four times per second.
2. **Optional VSync disable** — intercepts `eglSwapInterval` and changes the requested interval to zero only when the setting is enabled. Off by default.
3. **Experimental redundant program-bind cache** — can skip consecutive identical `glUseProgram` calls on the same EGL context. It is off by default and only becomes available if all context/program invalidation hooks install successfully.

The GL cache is intentionally opt-in. It does not claim to be a complete render-state cache. It may not help if Minecraft's renderer rarely repeats program binds, and it should be benchmarked on the target device before leaving it enabled.

## Not implemented yet

- Chunk/terrain rebuild optimization
- Entity and block-entity render batching/culling
- Particle culling
- Texture atlas and upload optimization
- Shader pipeline changes
- Minecraft-internal hooks or hard-coded offsets

Those need version-specific ARMv7 analysis of the exact patched `libminecraftpe.so`. No speculative game-memory patches are included.

## Build on GitHub Actions

Copy this folder to `native-mods/bedrock-sodium/` in your repository and copy `.github/workflows/build-bedrock-sodium.yml` to `.github/workflows/`.

The workflow installs Android NDK `28.2.13676358`, builds only `armeabi-v7a`, checks the resulting ELF header, and uploads the `.levipack` artifact.

To build locally on a machine with Android SDK/NDK, CMake 3.22+, and Ninja:

```bash
cmake -S . -B build-armv7 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=armeabi-v7a \
  -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_shared

cmake --build build-armv7 --target levi_package --parallel
```

The package appears in `build-armv7/bedrock_sodium-0.1.0-armeabi-v7a.levipack`.

## Important testing note

This environment currently has CMake and Clang but no Android NDK or SDK, so it cannot produce the ARMv7 `.so` here. The GitHub Actions build is the next build/compile validation step. Even a successful CI build does not replace a real in-game test on the POCO C3.
