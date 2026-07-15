# Speculor SDK

Prebuilt, self-contained development kit for writing plugins for the
[Speculor](https://github.com/speculor-app/speculor) multi-sensor pipeline
platform. Plugins are shared libraries (`.dll` / `.so` / `.dylib`) that export a
C ABI vtable and are loaded at runtime by the host application.

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

The ABI is versioned; build against the bundle that matches your target engine
release. Check the release notes for ABI changes between cycles.

## Quick start

1. **Download** the bundle for your OS and **extract** it anywhere, e.g. `~/speculor-sdk`.
2. Point CMake at it and pull in the SDK:

   ```cmake
   cmake_minimum_required(VERSION 3.24)
   project(my_plugin LANGUAGES CXX)

   find_package(SpeculorSDK REQUIRED)
   spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
   ```

3. **Configure** with `CMAKE_PREFIX_PATH` set to the extracted folder:

   ```bash
   cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/extracted/SpeculorSDK-<version>-<os>-<arch>
   cmake --build build
   ```

`spc_add_plugin()` (from the bundled `PluginHelpers.cmake`) builds a correctly
configured plugin shared library — no prefix, hidden visibility, RPATH wired for
the bundled dependencies. Drop the resulting library into the host application's
plugin directory to load it.

A complete, buildable example is in [`examples/minimal-plugin/`](examples/minimal-plugin/).

## Minimal plugin

```cpp
#include <speculor/plugin_helpers.h>

struct MyPluginState {
    spc::HostServices host;
    float gain = 1.0f;
};

SPC_PLUGIN(MyPluginState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Filters")
        .author("You").version("0.1.0")
        .description("A minimal example plugin")
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)
        .input("in", "Input", SPC_DATA_FRAME)
        .output("out", "Output", SPC_DATA_FRAME)
)

SPC_PLUGIN_AUTO_PARAMS(MyPluginState,
    SPC_BIND_FLOAT(MyPluginState, "gain", gain)
)

static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    // read inputs, write outputs
    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .set_host_services = set_host_services
)
```

```cmake
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

All plugin code uses a single include: `#include <speculor/plugin_helpers.h>`.

## What's in the bundle

| Component | Type | Description |
|-----------|------|-------------|
| **speculor_sdk** | Static library | Core SDK utilities (ring buffers, table helpers) |
| **spclib** | Static library | Computer vision library (BGS, tracking, optical flow, video I/O) |
| **speculor_gpu** | Shared library | Vulkan GPU acceleration — pipeline base, dispatch/staging/barrier helpers, RAII handle lifecycle, buffer registry |
| **SDK headers** | Headers | C ABI plugin interface + C++ convenience wrappers (`include/speculor/`) |
| **Shared plugin headers** | Headers | Plugin-side utilities (`include/speculor_common/`) — GPU helpers, shared clock, OpenCV bridge, UI panel toolkit |
| **CMake helpers** | Build tools | `spc_add_plugin()`, `spc_enable_gpu()`, `spc_add_gpu_shaders()`, `embed_spirv.cmake` |
| **glslangValidator** | Tool | GLSL → SPIR-V compiler for GPU plugin shaders |

### SDK headers

The public API lives in `include/speculor/`:

| Header | Purpose |
|--------|---------|
| `plugin_api.h` | Plugin vtable, host services, capability flags, GPU record context |
| `plugin_macros.h` | `SPC_PLUGIN_VTABLE` (designated-initializer vtable export) + author convenience macros |
| `plugin_helpers.h` | C++ convenience layer (DescriptorBuilder, macros, helpers) |
| `data_types.h` | Frame, Table, Record, Scalar, Signal, Packet, Event types |
| `parameter_types.h` | Parameter descriptors (INT, FLOAT, FLOAT64, BOOL, STRING, ENUM, COLOR, LIST, TRIGGER, DECIMAL) |
| `table_helpers.h` | Schema table field accessors and lifecycle |
| `ring_buffer.h` | Lock-free SPSC ring buffer for signal/audio data |
| `log_types.h` | Log levels and callback types |
| `plugin_log.h` | `SPC_LOG_*` convenience macros |
| `sdr_params.h` | SDR parameter standard definitions |
| `version.h` | SDK version macros |

`plugin_helpers.h` is the umbrella — it transitively pulls in `descriptor_builder.h`
(`DescriptorBuilder`), `param_dispatch.h`, `decimal_helpers.h`,
`control_msg_helpers.h`, `io_helpers.h`, and `sdr_source_helpers.h` (SDR source
parameter/scaffolding helpers).

### Data types

| Type | Enum | Description |
|------|------|-------------|
| Video frame | `SPC_DATA_FRAME` | Pixel buffer with format, stride, timestamp |
| Table | `SPC_DATA_TABLE` | Schema-described binary flat array (hot path) |
| Record | `SPC_DATA_RECORD` | JSON metadata string (cold path) |
| Scalar | `SPC_DATA_SCALAR` | Single typed value (float, int, bool, decimal) |
| Signal | `SPC_DATA_SIGNAL` | Direct-call zero-copy transport for high-throughput data |
| Control Message | `SPC_DATA_CONTROL_MSG` | Bidirectional control message — set/get/list params (control/feedback connections) |
| Packet | `SPC_DATA_PACKET` | Compressed bitstream passthrough (H.264/HEVC/MJPEG, pre-decode) |

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

Plugins cross a shared-library boundary, so the SDK enforces:

- **C linkage only** (`extern "C"`) at the ABI boundary
- **POD types only** — no C++ types cross the boundary
- **Fixed-size buffers** for strings and arrays
- **Vtable versioning** — `struct_size` as the first field in all extensible structs

Every plugin exports its vtable via a single `SPC_PLUGIN_VTABLE(...)` macro using
C++20 designated initializers. List only the slots your plugin implements;
unmentioned slots default to `nullptr`. Fields must appear in `SpcPluginVTable`'s
declaration order (see the `SpcPluginVTable` typedef in `plugin_api.h`). Common
slots include `.start` / `.stop` (streaming source), `.record_gpu` (coalesced GPU
submit), `.request_stop` (pre-drain abort for blocking I/O), `.on_input_event`
(interactive), `.scan_devices` (hardware enumeration), `.validate_mandatory`
(conditional mandatory params), `.get_list_rows` / `.set_list_rows` (list
parameters), `.on_signal` / `.on_control_response`.

Key ABI limits: `SPC_PORT_MAX` = 16 ports per plugin, `SPC_PLUGIN_PARAM_MAX` = 64
parameters per plugin, `SPC_PARAM_ENUM_MAX` = 32 options per enum parameter.

### Plugin descriptor flags

Descriptors carry an optional flag bitmask, set via `DescriptorBuilder`:

| Flag | Builder | Meaning |
|------|---------|---------|
| `SPC_PLUGIN_STATIC` (1&nbsp;<<&nbsp;0) | `.static_source()` | Process once, re-run only on a parameter change. |
| `SPC_PLUGIN_DATA_SOURCE` (1&nbsp;<<&nbsp;1) | `.data_source()` | The plugin ingests data from outside the pipeline (camera/SDR/GPS device, file, network stream, generator). Marks what a Sources-mode session recording captures and what reinjection replay drives. |
| `SPC_PLUGIN_LIVE_ONLY` (1&nbsp;<<&nbsp;2) | `.live_only()` | `process()` has an irreversible external effect (recorder, network egress, DB/service feed). The engine skips the node during reinjection replay so a recorded session is never re-broadcast. **Terminal sinks only.** |

The two egress-safety flags (`SPC_PLUGIN_DATA_SOURCE`, `SPC_PLUGIN_LIVE_ONLY`)
require SDK ≥ 0.20.0 and matter for recording/replay correctness — a source that
is not marked will not be captured or replayed, and an unmarked live sink will
re-broadcast recorded data during replay.

### Capability flags

Separate from the descriptor flags, capabilities tell the host which vtable slots
to call: `SPC_CAP_STREAMING` (1 << 0), `SPC_CAP_SIGNAL_INPUT` (1 << 1),
`SPC_CAP_FRAME_ALLOC` (1 << 2), `SPC_CAP_INTERACTIVE` (1 << 3),
`SPC_CAP_DEVICE_SCAN` (1 << 4), `SPC_CAP_GPU_COMPUTE` (1 << 5),
`SPC_CAP_GPU_OWN_SUBMIT` (1 << 6). Set them through the matching
`DescriptorBuilder` methods (`.streaming()`, `.gpu_compute()`, …) rather than by
hand.

### GPU plugins — coalesced submit

GPU plugins expose the `record_gpu` vtable slot to opt into engine-driven
coalesced submit: the engine fuses chains of GPU-active nodes on the same
`gpu_profile` into one `vkQueueSubmit2` per subgraph per frame. Plugins record
into engine-allocated secondary command buffers via
`GpuPipelineBase::ScopedExternalRecording`. **Eager init in `start()` is
mandatory** — `record_gpu` bypasses `process()`. The engine can run a subgraph
**K frames in flight** (the `pipeline_depth` setting, default 2, range 1–3): for
K≥2 it owns **K rotating per-frame data-path buffers per edge**, which the plugin
fetches via the `SpcHostServices::acquire_ringed_output` /
`acquire_ringed_upload` callbacks (at K=1, byte-identical to a plugin owning one
buffer).

### Two-phase shutdown

Plugins blocking on uninterruptible I/O (FFmpeg RTSP, blocking sockets) implement
the optional `request_stop` slot: the engine calls it BEFORE draining in-flight
`process()` / `record_gpu` so the plugin can flip atomic flags and kick interrupt
callbacks (FFmpeg `AVIOInterruptCB`, `shutdown(socket)`). `stop()` then runs
AFTER drain for actual teardown. Wire it via `.request_stop = request_stop` in the
`SPC_PLUGIN_VTABLE(...)` call; plugins without blocking I/O omit the slot
entirely.

## Documentation

- [**Plugin Development Guide**](docs/plugin-development.md) — the full authoring
  guide: lifecycle, descriptor, parameters, frames, tables, signals, GPU compute
  and coalesced submit, two-phase shutdown, the watchdog contract, and the
  complete SDK reference.
- [`examples/minimal-plugin/`](examples/minimal-plugin/) — a buildable
  frame-in / frame-out filter, and the self-containment check for a bundle.
- [speculor-plugin-examples](https://github.com/speculor-app/speculor-plugin-examples)
  — larger example plugins (sources, filters, a Vulkan compute shader, an FFT
  spectrometer).
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
