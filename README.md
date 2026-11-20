# Speculor SDK

Prebuilt, self-contained development kit for writing plugins for the
[Speculor](https://github.com/speculor-app/speculor) multi-sensor pipeline
platform. Plugins are shared libraries (`.dll` / `.so`) that export one C object,
`spc_plugin`, and are loaded at run time by the host application.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![CMake](https://img.shields.io/badge/CMake-3.24%2B-blue)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-lightgrey)
![Arch](https://img.shields.io/badge/Arch-x64%20%7C%20arm64-lightgrey)

> This repository hosts the **public SDK distribution** — documentation and the
> downloadable, prebuilt SDK bundles (under [**Releases**](../../releases)). The
> SDK source is developed in a separate private repository.

## Download

Grab the bundle for your platform from the [**latest Release**](../../releases/latest):

| Platform | Asset |
|----------|-------|
| Windows x64   | `SpeculorSDK-<version>-Windows-x64.zip` |
| Linux x64     | `SpeculorSDK-<version>-Linux-x86_64.tar.gz` |
| Linux arm64   | `SpeculorSDK-<version>-Linux-aarch64.tar.gz` |

Each bundle is **fully self-contained**: the SDK headers and libraries plus every
dependency (OpenCV, FFmpeg, Eigen, libjpeg-turbo, Vulkan loader + headers,
glslangValidator) and a standalone CMake package config. You need nothing beyond
a C++20 compiler and CMake 3.24+ — no vcpkg, no system packages.

The engine loads a plugin only when its ABI number (`SPC_ABI`) equals the
engine's own, so build against the bundle that matches the application release
you target. The release notes say when the number changes.

## Quick start

1. **Download** the bundle for your OS and **extract** it anywhere, e.g. `~/speculor-sdk`.
2. Point CMake at it and pull in the SDK:

   ```cmake
   cmake_minimum_required(VERSION 3.24)
   project(my_plugin LANGUAGES CXX)

   find_package(SpeculorSDK REQUIRED)
   include(PluginHelpers)      # spc_add_plugin()
   include(CompilerWarnings)   # spc_set_warnings(), which spc_add_plugin calls

   spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
   ```

   Both `include()` lines are required: `find_package` only puts the bundle's
   cmake directory on `CMAKE_MODULE_PATH`, it does not include the modules.

3. **Configure** with `CMAKE_PREFIX_PATH` set to the extracted folder:

   ```bash
   cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/extracted/SpeculorSDK-<version>-<os>-<arch>
   cmake --build build
   ```

`spc_add_plugin()` (from the bundled `PluginHelpers.cmake`) builds a correctly
configured plugin shared library — no prefix, hidden visibility, RPATH wired for
the bundled dependencies. Drop the resulting library into the host application's
plugin directory to load it.

A complete project to copy — plugin, CMakeLists and a conformance test — is in
[`templates/standalone-plugin/`](https://github.com/speculor-app/speculor-plugin-examples/tree/main/templates/standalone-plugin),
where CI configures it on its own against each published bundle.

## Minimal plugin

```cpp
#include <speculor/plugin.h>          // the boundary
#include <speculor/plugin_helpers.h>  // conveniences compiled into the plugin

#include <new>

enum Param : uint32_t { P_VALUE };   // parameters by position, in declaration order

SPC_PLUGIN_DESCRIPTOR(               // defines info()
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Sources/General")
        .run_on_change()
        .author("You").version("0.1.0")
        .help("Outputs a constant scalar value")
        .float_param("value", "Value", -1e6f, 1e6f, 0.0f, 0.1f)   // P_VALUE
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

struct State { float value = 0.0f; };

static SpcPluginInstance* start(SpcHost* /*host*/, const SpcValue* values)
{
    auto* s = new (std::nothrow) State{};
    if (!s) return nullptr;
    s->value = values[P_VALUE].f;
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    delete reinterpret_cast<State*>(instance);
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values, uint64_t)
{
    reinterpret_cast<State*>(instance)->value = values[P_VALUE].f;
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    SpcData& out = step->outputs[0];
    out.type = SPC_DATA_SCALAR;
    out.scalar = {};
    out.scalar.type = SPC_FIELD_FLOAT;
    out.scalar.f32 = reinterpret_cast<State*>(instance)->value;
    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
```

```cmake
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

`speculor/plugin.h` is the whole boundary, in C11. `speculor/plugin_helpers.h`
adds the C++ conveniences: the node info builder, the macros, logging, table
and control helpers.

## What's in the bundle

| Component | Type | Description |
|-----------|------|-------------|
| **speculor_sdk** | Static library | The C++ conveniences a plugin links: node info builder, parameter and control helpers, table helpers, ring buffers |
| **spclib** | Static library | Computer vision library (BGS, tracking, optical flow, video I/O) |
| **spclib_http** | Static library | A small HTTP client (`spclib::http`) whose requests can be cancelled |
| **speculor_gpu_plugin** | Static library | The Vulkan helpers a GPU plugin links (`spc_enable_gpu()`): pipeline base, dispatch, staging and barrier helpers, working on the device the host lends |
| **speculor_gpu** | Shared library | The host side of the GPU helpers, for executables such as test runners |
| **SDK headers** | Headers | The plugin boundary (`plugin.h`) and the C++ conveniences (`include/speculor/`) |
| **Shared plugin headers** | Headers | Plugin-side utilities (`include/speculor_common/`): GPU helpers, the shared clock, the OpenCV bridge, on-frame UI controls, map tiles, the test harness |
| **CMake helpers** | Build tools | `spc_add_plugin()`, `spc_add_plugin_test()`, `spc_enable_gpu()`, `spc_add_gpu_shaders()`, `embed_spirv.cmake` |
| **glslangValidator** | Tool | GLSL to SPIR-V compiler for GPU plugin shaders |

### SDK headers

The public API lives in `include/speculor/`:

| Header | Purpose |
|--------|---------|
| `plugin.h` | The whole plugin boundary, in C (C11 or later, `stdint.h` only): `SpcPlugin` and `SPC_EXPORT_PLUGIN`, `SpcNodeInfo`, `SpcHost`, `SpcStep`, `SpcGpuPass`, the data types, results, limits and `SPC_ABI` |
| `plugin_helpers.h` | The C++ conveniences in one include |
| `plugin_macros.h` | `SPC_PLUGIN_DESCRIPTOR`, `SPC_IF_VULKAN`, and `SPC_EXPORT_PLUGIN_SLOTS` with the composition layer's macros |
| `descriptor_builder.h` | `spc::DescriptorBuilder`, which fills the node info |
| `param_dispatch.h` | Logging through the host, control commands, `spc::HostServices`, the GPU ring wrappers, parameter helpers |
| `io_helpers.h` | Reading inputs and filling outputs of a step |
| `table_helpers.h` | Schema offsets and typed table accessors |
| `schema_storage.h` | A schema that owns its fields |
| `plugin_log.h` | `SPC_LOG_*` macros |
| `decimal_helpers.h` | Exact decimals (`SpcDecimal`) |
| `ring_buffer.h` | Lock-free single-producer, single-consumer ring buffer |
| `sdr_params.h` | Standard SDR parameter names |
| `version.h` | SDK version macros |

### Data types

| Type | Enum | Description |
|------|------|-------------|
| Frame | `SPC_DATA_FRAME` | Pixels with format, stride, timestamp; CPU memory and/or a GPU handle |
| Table | `SPC_DATA_TABLE` | Schema-described flat array of records, lent by the host |
| JSON | `SPC_DATA_JSON` | A JSON string, for metadata |
| Scalar | `SPC_DATA_SCALAR` | One typed value (float, int, bool, decimal) |
| Signal | `SPC_DATA_SIGNAL` | Sample blocks (audio, SDR) the host holds until the consumer takes them |
| Control | `SPC_DATA_CONTROL` | Commands to set, get or list the parameters of connected nodes |
| Packet | `SPC_DATA_PACKET` | Compressed bitstream before decoding (H.264/HEVC/MJPEG, audio) |

Pixel formats: `GRAY8`, `RGB24`, `BGR24`, `RGBA32`, `NV12`, `FLOAT32`, `GRAY16`, `RGB48`

### Computer vision library (spclib)

| Module | Description |
|--------|-------------|
| **bgs** | Background subtraction: MOG2, SubSENSE, ViBe, Weighted Moving Variance |
| **blobs** | Connected component blob detection |
| **flow** | Optical flow computation (Horn-Schunck) |
| **sort** | Multi-object tracking (SORT algorithm) |
| **video** | Video I/O via FFmpeg (reader/writer with metadata) |
| **utils** | PID controller, JPEG compression, profiling |

## ABI rules

- **One C header.** `speculor/plugin.h` declares everything that crosses the
  boundary, in C11. Everything else in the SDK is compiled into the plugin.
- **One object.** A plugin exports `SpcPlugin spc_plugin`, defined by
  `SPC_EXPORT_PLUGIN(...)` from the slots it implements, listed in `SpcPlugin`'s
  order; a slot left out is null. C++ plugins built on the SDK's composition
  layer (`create_instance`, `set_host_services`, ...) export with
  `SPC_EXPORT_PLUGIN_SLOTS(...)` instead.
- **One number.** `abi`, the first field of `spc_plugin`, must equal the
  engine's `SPC_ABI`, or the plugin is not loaded.
- **Plain C data.** Strings are nul-terminated `const char*`, arrays come with
  their counts, and whoever makes one owns it, within the limits `plugin.h`
  names.
- **An instance only while its node runs.** `start(host, values)` makes it,
  `stop(instance)` frees it.
- **Every host function is set.** Each one says which threads may call it, and
  the conformance harness checks every rule.

Key limits: `SPC_PORT_MAX` = 16 inputs and 16 outputs per node,
`SPC_PLUGIN_PARAM_MAX` = 64 parameters per node, `SPC_PARAM_ENUM_MAX` = 32
options per ENUM parameter.

### Node traits

| Trait | Builder | Meaning |
|-------|---------|---------|
| `SPC_NODE_RUN_ON_CHANGE` | `.run_on_change()` | Runs once, then again only when a parameter changes. |
| `SPC_NODE_SOURCE` | `.source()` | The data comes from outside the pipeline (a device, a file, a network stream, a generator). A recording captures it, and replay feeds it from the session. |
| `SPC_NODE_LIVE_ONLY` | `.live_only()` | `process()` has an effect outside the pipeline that cannot be undone (a recorder, network egress, a service feed). Replay skips the node, so a recorded session is never sent again. **Terminal sinks only.** |

A source without `SPC_NODE_SOURCE` is neither recorded nor replayed, and a live
sink without `SPC_NODE_LIVE_ONLY` sends its data again on every replay.

### GPU plugins

A plugin declares how it uses a GPU, one `SpcGpuUse`: `.gpu_compute()`
(`SPC_GPU_RECORDED`), `.gpu_own_submit()` (`SPC_GPU_OWN_SUBMIT`) or
`.gpu_own_api()` (`SPC_GPU_OWN_API`). The engine lends the node its Vulkan
device (`SpcHost::gpu`, null while the node's GPU is off). A RECORDED plugin
records its work in the `record_gpu` slot into the engine's command buffer,
and the engine submits once for a whole chain of GPU nodes. The engine can run
a chain several frames deep; the plugin fetches each frame's buffers through
the ring calls of the `SpcGpuPass` it is handed. A plugin declares RECORDED or
OWN_SUBMIT only where it implements `record_gpu`:
`SPC_IF_VULKAN(.gpu_compute())`.

### Two-phase shutdown

A plugin that can block on I/O (an RTSP stream, a socket, an HTTP request)
implements the optional `interrupt` slot. The engine calls it from another
thread, before it waits for the node's thread to leave the plugin, so the
plugin can set flags and cut the blocking call: FFmpeg's `AVIOInterruptCB`,
`spclib::http::RequestOptions::cancel`, `spc::map::DownloadQueue::interrupt()`,
a bounded wait on a socket. `stop()` then runs after the node's thread has left
the plugin, for the teardown.

## Documentation

- [**Plugin Development Guide**](docs/plugin-development.md) — the full authoring
  guide: the boundary, lifecycle and threads, the node info, parameters, data
  types, the clock, GPU plugins, two-phase shutdown, the C++ composition layer,
  testing, the watchdog, and the SDK's headers.
- [speculor-plugin-examples](https://github.com/speculor-app/speculor-plugin-examples)
  — a [standalone project template](https://github.com/speculor-app/speculor-plugin-examples/tree/main/templates/standalone-plugin)
  to copy, plus 13 example plugins (sources, filters, a Vulkan compute shader,
  an FFT spectrometer). All of them run the SDK's conformance checks in CI
  against the latest published bundle, so they are verified to load and honour
  the ABI, not merely to compile.
- Site · <https://speculor.app> · Docs · <https://speculor.app/docs> ·
  Community · <https://speculor.app/community>

## Related repositories

| Repository | Description |
|------------|-------------|
| [speculor](https://github.com/speculor-app/speculor) | Speculor application releases and downloads |
| [speculor-plugin-examples](https://github.com/speculor-app/speculor-plugin-examples) | Example plugins for learning the SDK |
| [speculor-plugins-oss](https://github.com/speculor-app/speculor-plugins-oss) | Open-source plugin collection (ViBe, SuBSENSE, rtl_sdr) |

## License

The Speculor Plugin SDK is proprietary software distributed under a limited-grant
license that permits using the SDK headers and prebuilt libraries solely to
develop plugins for the Speculor engine. Plugin developers may license their
resulting plugins under any terms they choose (including permissive, copyleft, or
proprietary). The SDK itself may not be redistributed as a stand-alone product.
Copyright © 2026 Fabio Barros. See [`LICENSE`](LICENSE) for the full grant and
restrictions.

The bundle ships an **LGPL-only** edition of FFmpeg (no `--enable-gpl`, no x264 /
x265, no `--enable-nonfree`). All bundled third-party dependencies are LGPL or
permissive; full attribution is in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Security policy: [`SECURITY.md`](SECURITY.md).
