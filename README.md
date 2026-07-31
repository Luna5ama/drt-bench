# drt-bench

Minimal Windows Vulkan bench for iterating on compute-based DRT shaders. It opens a centered 1280x720 borderless window and only submits a frame after input, shader, display mode, exposure, or window state changes. Floating tool windows plot sampled output pixels in RGB, xyY, CIE YUV, CIELUV, CIELAB, CIELCh, JzAzBz, and JzCzHz cubes. RGB and xyY open at startup; `/cube <type>` opens additional cube windows, and each cube can be closed independently. Each view uses its two chromatic axes horizontally and its luminance or lightness axis vertically, with numeric labels at both ends of every axis. Jz follows the decoded image peak; Az, Bz, and Cz use the CIE 1931 visible-gamut bounds at that peak. Drag a cube with the left mouse button to rotate it, and right-click to reset its view.

## License

The bench C++ and support files at the repository root are licensed under the MIT License; see [`LICENSE`](LICENSE). All files below [`shaders/ap`](shaders/ap) are GPL-3.0-only. [`shaders/ap/LICENSE`](shaders/ap/LICENSE) controls the Alpha-Piscium material in that directory.

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

## Startup options

```text
--drt <path> | -d <path>
--exr <path> | -e <path>
--fp16 <path>
--fp32 <path>
--width <x> | -w <x>
--height <y> | -h <y>
```

These perform the matching load commands at startup. `--width` and `--height`
set the initial window and swapchain size before raw input is checked or loaded.

```powershell
.\build\drt-bench.exe -d shaders\ap -e frame.exr -w 1920 -h 1080
```

## Commands

```text
/loadexr <path>
/loadfp16 <path>
/loadfp32 <path>
/loaddrt <directory>
/sdr
/hdr
/screenshot
/resize <x> <y>
/cube <rgb|xyy|cieyuv|cieluv|cielab|cielch|jzazbz|jzczhz>
```

Paths may contain spaces; surrounding quotes are optional. Raw files are tightly packed, little-endian, row-major RGBA values and must contain exactly `window_width * window_height * 4` f16 or f32 components. EXR dimensions come from the file.

`/hdr` prefers `VK_COLOR_SPACE_HDR10_ST2084_EXT` with `VK_FORMAT_A2B10G10R10_UNORM_PACK32`, then falls back through the best HDR surface formats reported by the active display. `/sdr` prefers an 8-bit UNORM swapchain with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`. The selected pair is printed to stdout.

Screenshots are saved in the current directory with timestamped names. SDR uses
lossless WebP. HDR uses a 16-bit RGB PNG tagged as full-range Rec. 2020/PQ with a
`cICP` chunk and requires the HDR10 ST2084 A2B10G10R10 swapchain format.

`/resize` sets the borderless window to the requested pixel width and height.

`/cube` opens a new cube window for the requested color space. Multiple windows of the same type may be open at once.

## Shader settings

The always-open **Shader Settings** tool window recognizes Iris-style numeric options declared in the loaded DRT's `main.glsl`:

```glsl
#define SETTING_WATER_ROUGHNESS 9.0//[4.0 4.5 5.0 5.5 6.0 6.5 7.0 7.5 8.0 8.5 9.0]
#define SETTING_SSS_SAMPLE_COUNT 6//[2 3 4 6 8 12 16]
```

Only numeric defines with a `//[...]` value list become controls. They must be defined directly in `main.glsl`; finding one in an included file is an error. Before compilation, ordinary expression settings are replaced with values from an injected std140 UBO at set 0, binding 2. Floating-point options remain floats and all-integer lists are exposed to GLSL as `int(...)`. Moving one of these sliders updates the UBO and redraws immediately without recompiling the shader.

Settings referenced by preprocessor directives remain literal macros. Changing one automatically recompiles the shader, allowing Iris-style `#if SETTING_NAME == value` branches to work alongside runtime UBO settings.

English UI text is loaded case-insensitively from `<DRT directory>\lang\en_US.lang`. Supported Iris keys are `option.<name>`, `option.<name>.comment`, `prefix.<name>`, `suffix.<name>`, and `value.<name>.<token>`. Missing entries fall back to the define name and source value token.

## Shader contract

DRT files are GLSL compute-shader bodies without a `#version` line. The bench prepends:

```glsl
#version 460
#define DRT_BENCH_SDR 1 // defined only in SDR mode
// or: #define DRT_BENCH_HDR 1
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D usam_inputTex;
layout(set = 0, binding = 1) uniform writeonly image2D uimg_outputTex;
layout(std140, set = 0, binding = 2) uniform DrtBenchSettings { ... } drtBenchSettings;
```

The shader supplies `main()`. `usam_inputTex` uses linear filtering with clamp-to-edge addressing. `uimg_outputTex` is the acquired swapchain image, so `imageSize(uimg_outputTex)` is the window size. In HDR10 mode the shader should write display-ready ST.2084 values.

`/loaddrt` loads `<directory>/main.glsl` as the entry point and uses `<directory>` as the shader include root: `#include "/util/Colors2.glsl"` resolves to `<directory>/util/Colors2.glsl`. Relative includes remain relative to the including file. It watches every file below that directory, including includes. Changes are debounced for 100ms; after a reload completes, the next reload waits at least 500ms. Successful recompiles redraw immediately; failed recompiles keep the last working pipeline.

## Alpha-Piscium DRT

`shaders\ap\main.glsl` accepts linear ACES AP0 input only. Set `DRT_BENCH_DRT` to `0` for AgX, `1` for OpenDRT, or `2` for Skibidi.

```text
/loaddrt shaders\ap
/loadexr <path>
```

The copied AgX, OpenDRT, and Skibidi core files are intentionally byte-for-byte unchanged; compatibility code lives outside them. For HDR, output bridges the cores' sRGB through linear Rec. 2020 to ST 2084, using 203 nits as SDR white.

## OpenDRT converter

`opendrt-convert.exe` accepts any number of EXR files, quoted wildcard patterns, and directories. Directories are scanned non-recursively for EXR files. Each input is converted with the standard OpenDRT look to a lossless WebP beside the source, replacing `.exr` with `.webp`.

```powershell
.\build\opendrt-convert.exe frame.exr "frames\*.exr" frames
.\build\opendrt-convert.exe --self-test
```
