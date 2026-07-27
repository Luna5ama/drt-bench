# drt-bench

Minimal Windows Vulkan bench for iterating on compute-based DRT shaders. It opens a centered 1280x720 borderless window and only submits a frame after input, shader, display mode, exposure, or window state changes.

## Build

Requirements: Visual Studio C++ Build Tools, Vulkan SDK (including `glslc`), CMake, Ninja, and vcpkg.

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

Run from a terminal so stdin remains available:

```powershell
.\build\drt-bench.exe
```

Press Escape or close the window to exit. F2 saves a screenshot, F5 reloads the
current DRT shader, and F6 toggles SDR/HDR.

## Commands

```text
/loadexr <path>
/loadfp16 <path>
/loadfp32 <path>
/loaddrt <path>
/sdr
/hdr
/screenshot
```

Paths may contain spaces; surrounding quotes are optional. Raw files are tightly packed, little-endian, row-major RGBA values and must contain exactly `window_width * window_height * 4` f16 or f32 components. EXR dimensions come from the file.

`/hdr` prefers `VK_COLOR_SPACE_HDR10_ST2084_EXT` with `VK_FORMAT_A2B10G10R10_UNORM_PACK32`, then falls back through the best HDR surface formats reported by the active display. `/sdr` prefers an 8-bit UNORM swapchain with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`. The selected pair is printed to stdout.

Screenshots are saved in the current directory with timestamped names. SDR uses
lossless WebP. HDR uses a 16-bit RGB PNG tagged as full-range Rec. 2020/PQ with a
`cICP` chunk and requires the HDR10 ST2084 A2B10G10R10 swapchain format.

## Shader contract

DRT files are GLSL compute-shader bodies without a `#version` line. The bench prepends:

```glsl
#version 460
#define DRT_BENCH_SDR 1 // defined only in SDR mode
// or: #define DRT_BENCH_HDR 1
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D usam_inputTex;
layout(set = 0, binding = 1) uniform writeonly image2D uimg_outputTex;
```

The shader supplies `main()`. `usam_inputTex` uses linear filtering with clamp-to-edge addressing. `uimg_outputTex` is the acquired swapchain image, so `imageSize(uimg_outputTex)` is the window size. In HDR10 mode the shader should write display-ready ST.2084 values.

After `/loaddrt`, the source file is polled for changes. Successful recompiles redraw immediately; failed recompiles keep the last working pipeline. Reload attempts are separated by at least one second.

## Alpha-Piscium DRT

`shaders\alpha-piscium-drt.glsl` accepts linear ACES AP0 input only. Set `DRT_BENCH_DRT` to `0` for AgX, `1` for OpenDRT, or `2` for Skibidi.

```text
/loaddrt shaders\alpha-piscium-drt.glsl
/loadexr <path>
```

The copied AgX, OpenDRT, and Skibidi core files are intentionally byte-for-byte unchanged; compatibility code lives outside them. For HDR, output bridges the cores' sRGB through linear Rec. 2020 to ST 2084, using 203 nits as SDR white.
