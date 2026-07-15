# Plugin Development Guide

Speculor plugins are shared libraries (`.dll` / `.so` / `.dylib`) that export a C ABI vtable. The host application discovers them at runtime in its plugin directory, so plugins can be built independently and dropped in without recompiling the host.

This guide walks through plugin development from a minimal example to advanced patterns. You will need:

- A C++20 compiler (MSVC 19.30+, GCC 12+, Clang 15+)
- CMake 3.24+
- An extracted **Speculor SDK bundle** — download it from the [Releases](https://github.com/speculor-app/speculor-sdk-dist/releases/latest) page. The bundle is fully self-contained: headers, prebuilt libraries, every dependency, and a standalone CMake package config. Nothing else is required.

Point CMake at the extracted bundle and pull the SDK in with `find_package`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_plugin LANGUAGES CXX)

find_package(SpeculorSDK REQUIRED)
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/extracted/SpeculorSDK-<version>-<os>-<arch>
cmake --build build
```

Header paths in this guide are relative to the bundle's include directories: `speculor/...` for the public SDK API (`include/speculor/`), and unprefixed paths such as `gpu/...`, `spc_clock.h`, or `cv_helpers.h` for the shared plugin headers (`include/speculor_common/`, added to your include path by `spc_add_plugin()`).

All plugin code uses a single include:

```cpp
#include <speculor/plugin_helpers.h>
```

This pulls in the C ABI definitions (`plugin_api.h`), logging macros (`plugin_log.h`), and the C++ convenience layer. Your plugin code lives entirely inside the shared library — the helpers are header-only and do not affect the ABI boundary.

---

## Your First Plugin

Here is a complete plugin that outputs a constant scalar value. It uses all three convenience macros and compiles to about 20 lines:

**`scalar_source_plugin.cpp`**

```cpp
#include <speculor/plugin_helpers.h>

// 1. define your state struct
struct ScalarSourceState {
    spc::HostServices host;      // host services (logging, frame alloc)
    float value = 0.0f;          // the parameter we expose
};

// 2. generate create/destroy/state()/set_host_services()
SPC_PLUGIN(ScalarSourceState, host)

// 3. declare the descriptor (metadata, ports, parameters)
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("scalar_source", "Scalar Source", "Sources")
        .static_source()
        .author("Speculor").version("0.1.0")
        .description("Outputs a constant scalar value")
        .float_param("value", "Value", -1e6f, 1e6f, 0.0f, 0.1f)
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

// 4. generate set_parameter() and get_parameter() from bindings
SPC_PLUGIN_AUTO_PARAMS(ScalarSourceState,
    SPC_BIND_FLOAT(ScalarSourceState, "value", value)
)

// 5. implement process — read inputs, write outputs
static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    if (output_count < 1) return -1;
    spc::set_scalar_output(outputs[0], state(inst)->value);
    return 0;
}

// 6. export the vtable
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

**`CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(scalar_source LANGUAGES CXX)

find_package(SpeculorSDK REQUIRED)
spc_add_plugin(scalar_source SOURCES scalar_source_plugin.cpp)
```

Line-by-line breakdown:

- **`SPC_PLUGIN(ScalarSourceState, host)`** generates four functions: `state()` (cast helper), `create_instance()`, `destroy_instance()`, and `set_host_services()`. The second argument is the name of the `spc::HostServices` member in your state struct.
- **`SPC_PLUGIN_DESCRIPTOR(...)`** generates a `get_descriptor()` function with thread-safe lazy initialization. The `DescriptorBuilder` fluent API sets metadata, ports, and parameters. Pass the builder expression directly — the macro calls `build_into()` for you, so do **not** terminate the chain with a `build()` call.
- **`SPC_PLUGIN_AUTO_PARAMS(...)`** generates `set_parameter()` and `get_parameter()` by binding parameter names directly to struct fields.
- **`SPC_PLUGIN_VTABLE(...)`** exports the `spc_plugin_vtable` symbol using C++20 designated initializers — list only the slots you implement, the rest default to `nullptr`. Fields must appear in `SpcPluginVTable` declaration order (`plugin_api.h`).

---

## How It Works

### Plugin Lifecycle

1. **Discovery** — The host scans its plugin directory for shared libraries. For each one, it loads the exported `spc_plugin_vtable` symbol.
2. **Descriptor** — The host calls `get_descriptor()` to read the plugin's metadata, ports, and parameters. This is called once.
3. **Instantiation** — The host calls `create_instance()` to allocate a new plugin instance. Multiple instances of the same plugin are allowed.
4. **Host Services** — If the vtable has a non-null `set_host_services`, the host calls it to inject logging and frame allocation callbacks.
5. **Parameters** — The host calls `set_parameter()` to push initial values and user changes. It calls `get_parameter()` to read current values.
6. **Start** — For streaming plugins (sources, cameras), the host calls `start()` before the first `process()`.
7. **Process** — The engine calls `process()` on each tick. Input data arrives in the `inputs` array; the plugin writes results to the `outputs` array.
8. **Stop** — The host calls `stop()` when the pipeline stops.
9. **Destruction** — The host calls `destroy_instance()` to free the plugin state.

### The VTable

Every plugin exports a single symbol `spc_plugin_vtable` of type `SpcPluginVTable`:

```c
typedef struct {
    uint32_t                 struct_size;        // = sizeof(SpcPluginVTable)
    SpcGetDescriptorFn       get_descriptor;
    SpcCreateInstanceFn      create_instance;
    SpcDestroyInstanceFn     destroy_instance;
    SpcSetParameterFn        set_parameter;
    SpcGetParameterFn        get_parameter;
    SpcProcessFn             process;
    SpcStartFn               start;              // NULL if not a source
    SpcStopFn                stop;               // NULL if not a source
    SpcSetHostServicesFn     set_host_services;   // NULL if not needed
    SpcOnSignalFn            on_signal;           // NULL if no signal inputs
    SpcOnInputEventFn        on_input_event;      // NULL if not interactive
    SpcScanDevicesFn         scan_devices;        // NULL if no device scanning
    SpcOnControlResponseFn   on_control_response; // NULL if no control responses
    SpcGetListRowsFn         get_list_rows;       // NULL if no list params
    SpcSetListRowsFn         set_list_rows;       // NULL if no list params
    SpcValidateMandatoryFn   validate_mandatory;  // NULL = host runs default flag-based check
    SpcRecordGpuFn           record_gpu;          // NULL = CPU-only or legacy non-coalesced
                                                  // GPU plugin. Non-NULL opts the plugin
                                                  // into engine-driven coalesced GPU submit
                                                  // (one vkQueueSubmit2 per subgraph per
                                                  // frame instead of one per plugin) — the
                                                  // standard path for all GPU plugins, see
                                                  // "GPU Compute Plugins" below.
    SpcRequestStopFn         request_stop;        // NULL = engine relies on stop_token + queue
                                                  // shutdown alone. Non-NULL = plugin gets an
                                                  // abort-signal call BEFORE drain. Required
                                                  // for plugins whose process()/record_gpu
                                                  // can block on uninterruptible I/O (FFmpeg
                                                  // av_read_frame on RTSP, blocking socket
                                                  // recv). See "Two-Phase Shutdown" below.
} SpcPluginVTable;
```

The `struct_size` field enables forward compatibility. If a future SDK version appends new function pointers to the vtable, the host uses `struct_size` to detect which fields are present. Older plugins compiled against an earlier SDK will have a smaller `struct_size` and the host will treat the missing fields as `NULL`.

### Capability Flags

Plugins declare their capabilities in the descriptor. The host uses these to determine which vtable functions to call:

```c
typedef enum {
    SPC_CAP_NONE         = 0,
    SPC_CAP_STREAMING    = 1 << 0,  // plugin has start/stop lifecycle
    SPC_CAP_SIGNAL_INPUT = 1 << 1,  // plugin has on_signal handler
    SPC_CAP_FRAME_ALLOC  = 1 << 2,  // plugin uses host frame pool for zero-copy output
    SPC_CAP_INTERACTIVE  = 1 << 3,  // plugin accepts mouse/keyboard events via on_input_event
    SPC_CAP_DEVICE_SCAN  = 1 << 4,  // plugin supports scan_devices() for hardware enumeration
    SPC_CAP_GPU_COMPUTE  = 1 << 5,  // plugin uses GPU compute (accepts/produces GPU-resident frames)
    SPC_CAP_GPU_OWN_SUBMIT = 1 << 6, // GPU node does its own queue submits (never coalesced)
} SpcPluginCaps;
```

Use `DescriptorBuilder::gpu_compute()` instead of setting `SPC_CAP_GPU_COMPUTE` manually — it also adds the engine-managed `gpu_enabled` parameter. See [GPU Compute Plugins](#gpu-compute-plugins) for details.

Use `DescriptorBuilder::gpu_own_submit()` for a GPU node that submits to **its own queue** — e.g. NVOFA hardware optical flow, which fence-waits on a dedicated hardware queue. The engine routes GPU-resident input and calls `record_gpu`, but never fuses the node into a coalesced subgraph, so it can't stall a fused chain at pipeline depth > 1. `gpu_own_submit()` implies `gpu_compute()`.

---

## The Descriptor

Use `spc::DescriptorBuilder` to construct your plugin's descriptor. The builder provides a fluent API:

```cpp
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Filters")
        .author("Your Name")
        .version("1.0.0")
        .description("Does something useful")
        // capabilities
        .streaming()             // sets SPC_CAP_STREAMING
        .frame_alloc()           // sets SPC_CAP_FRAME_ALLOC
        .static_source()         // sets SPC_PLUGIN_STATIC flag
        // ports
        .input("frame_in", "Image", SPC_DATA_FRAME)
        .output("frame_out", "Image Out", SPC_DATA_FRAME)
        // parameters
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)
)
```

Pass the builder expression straight into `SPC_PLUGIN_DESCRIPTOR` — the macro fills the descriptor in place via `build_into()`. There is no `build()` method to call at the end of the chain.

Two optional classification methods sit alongside the metadata above:

- **`.maturity(m)`** — `SPC_MATURITY_EXPERIMENTAL` / `_PREVIEW` / `_STABLE`. Drives the plugin browser's maturity filter; defaults to `EXPERIMENTAL` (the zero value).
- **`.license_tier(t)`** — the **minimum** license tier required to use the plugin: `SPC_LICENSE_COMMUNITY` (default) / `_PERSONAL` / `_INDIE` / `_TEAM` / `_ENTERPRISE`. At load time the host skips any plugin whose required tier is above the active license — it never loads and never appears in the browser. Omit it and the plugin defaults to `COMMUNITY`, available to everyone.

### Descriptor Flags

Separate from the capability flags, a descriptor carries a flag bitmask that tells the engine how the node behaves with respect to the pipeline boundary. Set them via `DescriptorBuilder`:

| Flag | Builder | Meaning |
|------|---------|---------|
| `SPC_PLUGIN_STATIC` (1 << 0) | `.static_source()` | Process once, then re-run only when a parameter changes. |
| `SPC_PLUGIN_DATA_SOURCE` (1 << 1) | `.data_source()` | The plugin ingests data from **outside** the pipeline (camera/SDR/GPS device, file, network stream, generator) rather than deriving output from its inputs. |
| `SPC_PLUGIN_LIVE_ONLY` (1 << 2) | `.live_only()` | `process()` has an irreversible effect outside the pipeline (recorder, network egress, DB/service feed). **Terminal sinks only.** |

The last two are the **replay egress-safety flags** (SDK ≥ 0.20.0), and getting them right matters for recording/replay correctness:

- `.data_source()` marks what a Sources-mode session recording captures, and what reinjection replay drives. A source that omits the flag is not recorded and not replayed.
- `.live_only()` makes the engine skip the node during reinjection replay, so replaying a recorded session never re-broadcasts or re-uploads its data. A live sink that omits the flag will re-emit externally on every replay.

### Port Types

| Method | Data Type | Use Case |
|--------|-----------|----------|
| `.input(name, display, SPC_DATA_FRAME)` | Frame | Video/image data |
| `.output(name, display, SPC_DATA_FRAME)` | Frame | Video/image data |
| `.input(name, display, SPC_DATA_SCALAR)` | Scalar | Single float values (FPS, temperature) |
| `.output(name, display, SPC_DATA_SCALAR)` | Scalar | Single float values |
| `.input_table(name, display, fields)` | Table | Binary flat array (hot path) |
| `.output_table(name, display, fields)` | Table | Binary flat array |
| `.input_signal(name, display, fields)` | Signal | Direct-call zero-copy data (high throughput) |
| `.output_signal(name, display, fields)` | Signal | Direct-call zero-copy data |
| `.input(name, display, SPC_DATA_RECORD)` | Record | JSON metadata (cold path) |
| `.output(name, display, SPC_DATA_RECORD)` | Record | JSON metadata |
| `.output_control(name, display)` | Control Msg | Inter-plugin parameter control (`SPC_DATA_CONTROL_MSG`). `.output_param_cmd(name, display)` is a synonym kept for older plugin sources. |

For table, signal, and record ports, pass the schema as a field list:

```cpp
.output_table("detections", "Detections", {
    {"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT},
    {"w", SPC_FIELD_FLOAT}, {"h", SPC_FIELD_FLOAT},
    {"confidence", SPC_FIELD_FLOAT},
})
```

Input ports can specify a queue capacity and consume mode:

```cpp
.input_table("boxes_in", "Boxes", {
    {"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT},
    {"w", SPC_FIELD_FLOAT}, {"h", SPC_FIELD_FLOAT},
}, 4, SPC_CONSUME_LATEST)
```

Consume modes:
- `SPC_CONSUME_DEFAULT` — engine decides
- `SPC_CONSUME_FIFO` — pop every frame in order (blocking)
- `SPC_CONSUME_LATEST` — skip to newest, drop intermediates (blocking)
- `SPC_CONSUME_NON_BLOCKING` — try_pop, for static/optional inputs. Nodes with **only** NON_BLOCKING inputs are classified as source nodes and run on their own wall-clock timing instead of being input-driven.

Queue capacity applies to FIFO and NON_BLOCKING ports. `SPC_CONSUME_LATEST`
ports run a 1-slot queue regardless of the declared capacity: the consumer
only ever takes the newest item, and a deeper queue would just pin pool
frames while the node is blocked on another port.

`SPC_CONSUME_NON_BLOCKING` is the canonical pattern for **side-channel inputs** that update plugin state without gating `process()` — for example a `gps_in` port on a decoder that latches the receiver's position into its output, or on a camera source that stamps the camera's location into the emitted metadata record. The plugin polls the port each tick — when no fresh sample is available, the cached last-known-good value remains in effect.

### Parameter Methods

| Method | State Field Type | Arguments |
|--------|-----------------|-----------|
| `.int_param(name, display, min, max, default, step, group)` | `int32_t` | `group` is optional |
| `.float_param(name, display, min, max, default, step, group)` | `float` | `group` is optional |
| `.bool_param(name, display, default, group)` | `int32_t` (0 or 1) | `group` is optional |
| `.string_param(name, display, default, group)` | `char[SPC_PARAM_STRING_MAX]` | `group` is optional |
| `.enum_param(name, display, {"A", "B", "C"}, default_index, group)` | `int32_t` (index) | `group` is optional |
| `.color_param(name, display, default_rgba, group)` | `uint32_t` (0xRRGGBBAA) | `group` is optional |
| `.float64_param(name, display, min, max, default, step, group)` | `double` | `group` is optional |
| `.decimal_param(name, display, min, max, default, step, group)` | `SpcDecimal` (16 bytes) | All args are `SpcDecimal`; `group` is optional |
| `.trigger_param(name, display, group)` | — (momentary) | `group` is optional |
| `.text_param(name, display, default, group)` | `char[SPC_PARAM_TEXT_MAX]` | Multi-line text (SQL, scripts); `group` is optional |
| `.list_param(name, display, group)` | structured list | Chain `.list_column()` calls after |
| `.param_description(text)` | — | Sets tooltip on the previously added parameter |

The `group` parameter is an optional string that groups parameters together in the UI. Omit it to use the default group.

Chain `.param_description()` after any parameter method to add a tooltip shown on hover in the UI:
```cpp
.int_param("threshold", "Threshold", 1, 255, 50, 1, "Detection")
    .param_description("Pixel intensity difference to classify as foreground")
```

---

## Plugin State & Macros

The SDK provides a hierarchy of macros for generating boilerplate. Choose the level of control you need.

### `SPC_PLUGIN(State, host)` — The One-Liner

For most plugins, this is all you need. It generates four functions:

```cpp
struct MyState {
    spc::HostServices host;    // required: name must match the macro argument
    int32_t threshold = 50;
};

SPC_PLUGIN(MyState, host)
// generates:
//   static MyState* state(SpcPluginInstance*)           — cast helper
//   static SpcPluginInstance* create_instance()         — new MyState{}
//   static void destroy_instance(SpcPluginInstance*)    — delete state(inst)
//   static void set_host_services(SpcPluginInstance*,   — injects services
//                                  const SpcHostServices*)
```

Your state struct must be default-constructible. Use member initializers for default values.

### `SPC_PLUGIN_CAST(State)` + `SPC_PLUGIN_HOST_SERVICES(State, host)` — Custom Create/Destroy

When you need to do work in create or destroy (allocate tables, open files, precompute offsets), use the split macros:

```cpp
SPC_PLUGIN_CAST(MyState)
SPC_PLUGIN_HOST_SERVICES(MyState, host)

static SpcPluginInstance* create_instance() {
    auto* s = new MyState{};
    // custom initialization here
    spc_table_init(&s->output_table, s->stride, &get_descriptor()->ports[0].schema);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst) {
    auto* s = state(inst);
    spc_table_free(&s->output_table);
    delete s;
}
```

`SPC_PLUGIN_CAST` generates only the `state()` cast helper. `SPC_PLUGIN_HOST_SERVICES` generates only `set_host_services()`. You write `create_instance()` and `destroy_instance()` yourself.

### `SPC_PLUGIN_STATE(State)` — No Host Services

For plugins that do not need logging or frame allocation:

```cpp
SPC_PLUGIN_STATE(MyState)
// generates: state(), create_instance(), destroy_instance()
// does NOT generate set_host_services()
```

`SPC_PLUGIN_VTABLE` only wires the slots you list. If your plugin does not need host services, omit the `.set_host_services` entry — the engine treats a null slot as "this plugin does not consume host services."

---

## Parameters

There are three approaches to handling parameters, from simplest to most flexible.

### Approach 1: `SPC_PLUGIN_AUTO_PARAMS` — Pure Dispatch

Use this when parameter changes have no side effects. The macro generates both `set_parameter()` and `get_parameter()` that directly read/write struct fields:

```cpp
struct MyState {
    spc::HostServices host;
    float brightness = 0.0f;
    int32_t threshold = 50;
    int32_t enabled = 1;           // bool params are int32_t (0 or 1)
    int32_t mode = 0;              // enum params are int32_t (index)
    char label[SPC_PARAM_STRING_MAX] = "default";
    uint32_t tint_color = 0xFF0000FF;  // color params are uint32_t (0xRRGGBBAA)
};

SPC_PLUGIN(MyState, host)

SPC_PLUGIN_AUTO_PARAMS(MyState,
    SPC_BIND_FLOAT(MyState, "brightness", brightness),
    SPC_BIND_INT(MyState, "threshold", threshold),
    SPC_BIND_BOOL(MyState, "enabled", enabled),
    SPC_BIND_ENUM(MyState, "mode", mode),
    SPC_BIND_STRING(MyState, "label", label),
    SPC_BIND_COLOR(MyState, "tint", tint_color)
)
```

Each `SPC_BIND_*` macro creates a `spc::ParamBinding` that maps a parameter name to a field offset. The generated functions use `offsetof` to read/write the correct field. `SPC_BIND_STRING` also captures the field's capacity via `sizeof`, so the bound `char` array can be any size — writes are truncated and null-terminated to fit. Hand-written `ParamBinding` entries (without the macro) leave the capacity at 0, which assumes the field is `char[SPC_PARAM_STRING_MAX]`.

### Approach 2: `try_set_*` / `try_get_*` — Side Effects on Change

When a parameter change needs to trigger work (reallocate a buffer, reopen a device, update dependent state), write `set_parameter` and `get_parameter` manually using the `try_set_*` / `try_get_*` helpers:

```cpp
static int set_parameter(SpcPluginInstance* inst, const char* name,
                          const SpcParameterDesc* value) {
    auto* s = state(inst);

    // target_fps has no side effects — just assign
    if (spc::try_set_float(name, value, "target_fps", s->target_fps)) return 0;

    // width/height trigger buffer reallocation
    int32_t w = static_cast<int32_t>(s->width);
    int32_t h = static_cast<int32_t>(s->height);
    bool resized = spc::try_set_int(name, value, "width", w)
                || spc::try_set_int(name, value, "height", h);
    if (!resized) return -1;

    s->width = static_cast<uint32_t>(w);
    s->height = static_cast<uint32_t>(h);
    s->buffer.resize(s->width * s->height * 3);   // side effect
    return 0;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                          SpcParameterDesc* out) {
    auto* s = state(inst);
    if (spc::try_get_int(name, out, "width", static_cast<int32_t>(s->width))) return 0;
    if (spc::try_get_int(name, out, "height", static_cast<int32_t>(s->height))) return 0;
    if (spc::try_get_float(name, out, "target_fps", s->target_fps)) return 0;
    return -1;
}
```

The helper function signatures:

| Setter | Signature |
|--------|-----------|
| `try_set_int` | `bool try_set_int(const char* name, const SpcParameterDesc* value, const char* param_name, int32_t& field)` |
| `try_set_float` | `bool try_set_float(const char* name, const SpcParameterDesc* value, const char* param_name, float& field)` |
| `try_set_bool` | `bool try_set_bool(const char* name, const SpcParameterDesc* value, const char* param_name, int32_t& field)` |
| `try_set_enum` | `bool try_set_enum(const char* name, const SpcParameterDesc* value, const char* param_name, int32_t& field)` |
| `try_set_string` | `bool try_set_string(const char* name, const SpcParameterDesc* value, const char* param_name, char* field)` |
| `try_set_color` | `bool try_set_color(const char* name, const SpcParameterDesc* value, const char* param_name, uint32_t& field)` |

| Getter | Signature |
|--------|-----------|
| `try_get_int` | `bool try_get_int(const char* name, SpcParameterDesc* out, const char* param_name, int32_t value)` |
| `try_get_float` | `bool try_get_float(const char* name, SpcParameterDesc* out, const char* param_name, float value)` |
| `try_get_bool` | `bool try_get_bool(const char* name, SpcParameterDesc* out, const char* param_name, int32_t value)` |
| `try_get_enum` | `bool try_get_enum(const char* name, SpcParameterDesc* out, const char* param_name, int32_t value)` |
| `try_get_string` | `bool try_get_string(const char* name, SpcParameterDesc* out, const char* param_name, const char* value)` |
| `try_get_color` | `bool try_get_color(const char* name, SpcParameterDesc* out, const char* param_name, uint32_t value)` |

Each setter returns `true` if the name matched and the field was assigned. Each getter returns `true` if the name matched and the output was written.

### Approach 3: Manual `strcmp` — Maximum Control

For C-only plugins or when you want complete control:

```cpp
static int set_parameter(SpcPluginInstance* inst, const char* name,
                          const SpcParameterDesc* value) {
    MyState* s = (MyState*)inst;
    if (strcmp(name, "threshold") == 0 && value->type == SPC_PARAM_INT) {
        s->threshold = value->int_val.value;
        return 0;
    }
    return -1;
}
```

---

## Data Types

The `SpcData` union carries data between nodes:

```c
typedef struct {
    SpcDataType type;
    union {
        SpcFrame*      frame;        // video/image data
        SpcTable*      table;        // binary flat array (hot path)
        SpcRecord*     record;       // JSON metadata (cold path)
        SpcControlMsg* control_msg;  // inter-plugin parameter control
        SpcPacket*     packet;       // compressed bitstream (pre-decode)
        SpcScalar      scalar;       // single typed value
    };
} SpcData;
```

| Type | Enum | When to Use |
|------|------|-------------|
| Frame | `SPC_DATA_FRAME` | Video frames, images, depth maps |
| Scalar | `SPC_DATA_SCALAR` | Single float values (FPS, temperature, counters) |
| Table | `SPC_DATA_TABLE` | Structured binary data on the hot path (detections, tracks) |
| Record | `SPC_DATA_RECORD` | JSON metadata on the cold path (configuration, events) |
| Signal | `SPC_DATA_SIGNAL` | High-throughput zero-copy data (audio samples, SDR IQ) |
| Control Msg | `SPC_DATA_CONTROL_MSG` | Bidirectional control message — set / get / list params on a downstream node |
| Packet | `SPC_DATA_PACKET` | Compressed bitstream captured pre-decode (encode-free video recording/forwarding). Annex-B H.264/HEVC or MJPEG; `KEYFRAME`/`CONFIG`/`DISCONTINUITY` flags (`AUDIO` + `sample_rate`/`channels` reserved for compressed-audio passthrough); on a queue gap consumers discard until the next keyframe. Packet inputs always consume FIFO |

---

## Timestamps and the shared clock

Every `SpcFrame` and `SpcTable` carries `int64_t timestamp_ns` — **disciplined UTC nanoseconds** from the engine's one shared clock. Because all plugins read the same clock, timestamps are directly comparable across sources, which is what makes multi-sensor fusion, triangulation, and federation between Speculor instances correct.

Stamp your output from the shared clock via `spc_clock.h`:

```cpp
#include <spc_clock.h>

out->timestamp_ns = spc::clock::now_utc_ns(s->host);   // disciplined UTC ns
```

- `spc::clock::now_utc_ns(s->host)` — disciplined UTC ns; use this for the data-plane `timestamp_ns`.
- `spc::clock::now_mono_ns(s->host)` — monotonic ns (never steps back); use for intervals/pacing, **not** for the timestamp field. Keep `std::chrono::steady_clock` for sleeps/FPS — only the emitted timestamp comes from the shared clock.
- **Filters** should propagate the input's timestamp (`out->timestamp_ns = in->timestamp_ns;`) rather than re-stamp.
- A GPS+PPS or NTP source can discipline the shared clock for everyone via `spc::clock::discipline(s->host, &correction)`.

Every call degrades gracefully on an older host (falls back to the system clock), so a plugin built against this SDK still runs — unsynchronized — on a pre-0.14 host.

---

## Working with Frames

### Pixel Formats

```c
typedef enum {
    SPC_PIXEL_FORMAT_UNKNOWN = 0,
    SPC_PIXEL_FORMAT_GRAY8,        // 1 channel, 8-bit
    SPC_PIXEL_FORMAT_RGB24,        // 3 channels, 8-bit each
    SPC_PIXEL_FORMAT_BGR24,        // 3 channels, 8-bit (legacy, prefer RGB24)
    SPC_PIXEL_FORMAT_RGBA32,       // 4 channels, 8-bit each
    SPC_PIXEL_FORMAT_NV12,         // YUV 4:2:0 semi-planar
    SPC_PIXEL_FORMAT_FLOAT32,      // 1 channel, 32-bit float (depth maps, masks)
    SPC_PIXEL_FORMAT_GRAY16,       // 1 channel, 16-bit unsigned
    SPC_PIXEL_FORMAT_RGB48,        // 3 channels, 16-bit each
} SpcPixelFormat;
```

### Reading Input Frames

Use the `spc::input_frame()` helper to safely extract a frame from the inputs array:

```cpp
static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    const auto* frame = spc::input_frame(inputs, input_count, 0);
    if (!frame) return -1;

    // access pixel data
    // frame->data     — pointer to pixel buffer
    // frame->width    — width in pixels
    // frame->height   — height in pixels
    // frame->stride   — bytes per row (may include padding)
    // frame->format   — pixel format enum
    // ...
}
```

### Zero-Copy Output with the Frame Pool

Plugins that declare `.frame_alloc()` in their descriptor can acquire frames directly from the engine's pool, avoiding a copy on the output path:

```cpp
// try to acquire a pool frame
SpcFrame* out = s->host.acquire_frame(0, width, height, SPC_PIXEL_FORMAT_RGB24);

if (out) {
    // write directly into pool buffer
    render_into(out->data, out->stride, out->width, out->height);
    out->frame_number = s->frame_count++;
    spc::set_frame_output(outputs[0], out);
} else {
    // fallback: use a local buffer
    // the engine will copy the data when it sees pool_id == 0
    render_into(s->buffer.data(), width * 3, width, height);
    s->local_frame.data = s->buffer.data();
    s->local_frame.width = width;
    s->local_frame.height = height;
    s->local_frame.stride = width * 3;
    s->local_frame.format = SPC_PIXEL_FORMAT_RGB24;
    s->local_frame.frame_number = s->frame_count++;
    spc::set_frame_output(outputs[0], &s->local_frame);
}
```

Always handle the fallback case — `acquire_frame` may return `nullptr` if the pool is exhausted. If you acquire a frame but decide not to use it as output, release it with `s->host.release_frame(frame)`.

### GPU Compute Plugins

Frames can stay on GPU between GPU-capable plugins, eliminating PCIe round-trips. The engine handles transfers automatically:

- **GPU→GPU**: device-to-device copy (no PCIe transfer)
- **GPU→CPU**: instant memcpy from pre-populated staging buffer (no extra GPU submission)
- **CPU→GPU**: engine auto-uploads once for fan-out to multiple GPU consumers

#### Coalesced GPU submit (the standard path)

When the engine identifies a chain of GPU-active nodes on the same `gpu_profile` that all expose the `record_gpu` vtable slot, it runs them as a **coalesced subgraph**:

- One primary `VkCommandBuffer` per subgraph per frame, owned by the engine.
- One secondary `VkCommandBuffer` per plugin, allocated by the engine.
- Each plugin records its dispatches into its own secondary via `record_gpu`.
- The engine concatenates the secondaries, inserts cross-plugin barriers from the graph's edge metadata, and calls `vkQueueSubmit2` once per subgraph per frame.

This replaces N driver round-trips per frame (one per plugin) with 1, and eliminates a class of NVIDIA driver-pressure crashes on Windows when multiple compute submitters fire concurrently.

`record_gpu` is the standard GPU plugin path — background subtractors, optical flow, warps/dewarpers, and GPU-accelerated video sources all use it. Legacy in-plugin `submit_and_wait` / `submit_and_spin` is reserved for one-shot init paths (model warmup, prepare-on-first-frame).

**Optional fp16 compute storage.** `VulkanContext` exposes `has_float16`, set only when the device supports both `storageBuffer16BitAccess` and `shaderFloat16`. A compute plugin can store its work buffers as `float16_t` to halve bandwidth, gating on `has_float16` with an fp32/CPU fallback when it's absent — device creation never fails on hardware that lacks the extensions.

#### GPU pipeline depth (frames in flight)

The engine runs each coalesced subgraph **K frames deep** (the *GPU pipeline depth*, default 2, range 1–3; Preferences → Performance, or `--pipeline-depth=N` on the CLI), overlapping the CPU recording/upload of frame N+1 with the GPU compute of frame N. Per-frame data-path buffers come from an engine-owned, K-slot edge-buffer ring; the plugin acquires the current slot inside `record_gpu` (see [Producing GPU-Resident Output](#producing-gpu-resident-output)). At K=1 the ring is a single buffer, byte-identical to the pre-pipelined path.

A node declared `gpu_own_submit()` (`SPC_CAP_GPU_OWN_SUBMIT`) is never coalesced — it submits to its own queue and fence-waits there — but it still acquires its GPU-resident outputs from the engine ring so downstream GPU consumers read them without a re-upload.

#### Setting Up a GPU Plugin

Use `gpu_compute()` in the descriptor — it adds `SPC_CAP_GPU_COMPUTE` and an engine-managed `gpu_enabled` parameter (stop-only toggle, grayed out while running):

```cpp
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_filter", "My GPU Filter", "Filters")
        .gpu_compute()      // adds GPU capability + gpu_enabled param
        .frame_alloc()      // zero-copy output
        .input("in", "Input", SPC_DATA_FRAME)
        .output("out", "Output", SPC_DATA_FRAME)
)
```

#### GPU Pipeline Class

GPU compute pipelines inherit from `GpuPipelineBase` which provides per-pipeline command buffers, timeline semaphores, and helper methods that eliminate most Vulkan boilerplate. The class exposes both modes:

- **External-cmd-buffer mode** (`begin_external_recording` / `end_external_recording`, or the `ScopedExternalRecording` RAII wrapper) — the coalesced path. The engine begins a secondary, you record into it via the same barrier / dispatch / staging helpers, the engine ends and submits.
- **Owned-cmd-buffer mode** (`init_base` / `begin_recording` / `submit_and_wait` / `submit_and_spin`) — the legacy non-coalesced path. Used for one-shot init and for plugins that legitimately can't coalesce.

A typical GPU plugin implements both — `record(cmd, ...)` for the coalesced path (called from the plugin's `record_gpu` callback) and a small `run()` for one-shot init only:

```cpp
#include <gpu/gpu_pipeline_base.h>

class MyGpuPipeline : public spc::gpu::GpuPipelineBase {
public:
    bool init(spc::gpu::VulkanContext& ctx) {
        if (!init_base(ctx)) return false;   // owned cmd_buf + timeline (for run() only)
        // create descriptor layout, pipeline, buffers...
        return true;
    }

    // Coalesced path: record into the engine-allocated secondary command buffer.
    // The plugin's record_gpu callback calls this with cmd = the secondary.
    // out_device / out_staging come from the engine's edge ring (acquire_ringed
    // _output). out_staging is null on a pure GPU→GPU edge — skip the download.
    bool record(spc::gpu::VulkanContext& ctx, VkCommandBuffer cmd,
                const MyPushConstants& params,
                VkBuffer gpu_input, VkBuffer out_device,
                VkBuffer out_staging) {
        spc::gpu::GpuPipelineBase::ScopedExternalRecording scope(*this, cmd);
        cmd_upload_input(staging_in_, input_buf_, input_bytes_, gpu_input);
        barrier_transfer_to_compute();
        cmd_dispatch_compute(ctx, pipeline_, shader_,
                             layout_, desc_set_,
                             push_bufs_, push_sizes_, NUM_BUFFERS,
                             params, groups_x, groups_y);
        if (out_staging) {
            barrier_compute_to_transfer();
            cmd_download_to_staging(out_device, out_staging, output_bytes_);
        }
        return true;  // engine submits and waits
    }

    // Owned path: only used for one-shot init / warmup. Don't call this
    // from process() on a coalesced plugin — it submits its own command
    // buffer and steps on the engine's submit cadence.
    bool run_init(spc::gpu::VulkanContext& ctx, /* ... */) {
        if (!begin_recording(ctx)) return false;
        // record dispatches...
        return submit_and_spin(ctx);
    }

    const StagingBuffer& output_staging() const { return staging_out_; }

    void destroy(spc::gpu::VulkanContext& ctx) {
        destroy_base(ctx);
    }
};
```

The plugin's `record_gpu` callback is the bridge from the vtable into the pipeline:

```cpp
static int record_gpu(SpcPluginInstance* inst, SpcGpuRecordCtx* rctx) {
    auto* s = state(inst);
    if (rctx->struct_size < sizeof(SpcGpuRecordCtx)) return -1;

    auto cmd = static_cast<VkCommandBuffer>(rctx->cmd_buffer_handle);
    auto* in = (rctx->input_count > 0 && rctx->inputs[0].type == SPC_DATA_FRAME)
                   ? rctx->inputs[0].frame : nullptr;
    if (!in) return SPC_NO_DATA;

    auto* out = s->host.acquire_gpu_frame(0, in->width, in->height, in->format);
    if (!out) return -1;

    // edge_ring_ctx is null only for a non-coalesced (own-submit / legacy) node.
    if (!rctx->edge_ring_ctx) { s->host.release_frame(out); return -1; }

    VkBuffer gpu_input = VK_NULL_HANDLE;
    if (in->gpu_handle) {
        if (auto* e = spc::gpu::GpuBufferRegistry::instance().lookup(in->gpu_handle))
            gpu_input = e->buffer;
    }

    // Acquire this frame's slot from the engine's K-deep output ring.
    SpcGpuEdgeBuffer ob = s->host.acquire_ringed_output(
        rctx->edge_ring_ctx, /*port*/ 0, out->width, out->height, out->stride,
        out->format, s->pipeline.output_device_size(),
        s->pipeline.output_staging_size());
    if (!ob.device_buffer || ob.gpu_handle == 0) {
        s->host.release_frame(out);
        return -1;
    }

    // Record compute into the engine's secondary, writing to ob.device_buffer.
    // A non-null ob.staging_buffer means this edge has a CPU consumer this
    // frame — record the device→staging download only then.
    if (!s->pipeline.record(*s->vk_ctx, cmd, s->params, gpu_input,
                            static_cast<VkBuffer>(ob.device_buffer),
                            static_cast<VkBuffer>(ob.staging_buffer))) {
        s->host.release_frame(out);
        return -1;
    }

    // Publish: the engine owns the slot's lifecycle — no GpuOutputHandle.
    out->gpu_handle   = ob.gpu_handle;
    out->gpu_flags   |= SPC_GPU_FLAG_RESIDENT;
    out->frame_number = in->frame_number;
    out->timestamp_ns = in->timestamp_ns;
    rctx->outputs[0].type  = SPC_DATA_FRAME;
    rctx->outputs[0].frame = out;
    return 0;
}
```

**Eager init in `start()` is required for coalesced plugins.** `record_gpu` bypasses `process()`, so any GPU init that previously lived inside `process()` (lazy `vk_ctx = get_shared(); pipeline.init(...)` on the first frame) will never run. Move it to `start()`:

```cpp
static int start(SpcPluginInstance* inst) {
    auto* s = state(inst);
    s->vk_ctx = spc::gpu::VulkanContext::get_shared();
    if (s->vk_ctx && s->pipeline.init(*s->vk_ctx))
        s->gpu_available = true;
    return 0;
}
```

If `gpu_available` stays false because eager init was skipped, `record_gpu` returns -1 every frame, the engine cancels the subgraph, and the upstream pool exhausts within seconds.

**Key helpers** (all protected on `GpuPipelineBase`):

| Method | Replaces |
|--------|----------|
| `ScopedExternalRecording` | RAII wrapper around `begin_external_recording` / `end_external_recording` |
| `cmd_dispatch_compute<T>()` | 12-line bind + push constants + dispatch block |
| `cmd_bind_compute()` | shader_object vs pipeline + push_desc vs descriptor_set branching |
| `cmd_upload_input()` | VkBufferCopy + conditional source (staging vs device) |
| `cmd_download_to_staging()` | VkBufferCopy for output readback |
| `download_output()` | staging invalidate + conditional memcpy |
| `barrier_compute_to_compute()` | manual VkMemoryBarrier for multi-pass |
| `copy_to_staging()` / `copy_from_staging()` | stride-aware staging memcpy |
| `begin_timing()` / `TimingScope` | manual chrono measurement |

#### Accepting GPU-Resident Input

Check `gpu_handle` on the input frame. If set, the frame data is on GPU — use `GpuBufferRegistry::device_copy()` instead of staging upload:

```cpp
#include <gpu/gpu_buffer_registry.h>

// in process():
bool input_on_gpu = in->gpu_handle != 0 &&
                    (in->gpu_flags & SPC_GPU_FLAG_RESIDENT);

VkBuffer gpu_input = VK_NULL_HANDLE;
if (input_on_gpu) {
    auto* entry = spc::gpu::GpuBufferRegistry::instance().lookup(in->gpu_handle);
    if (entry) gpu_input = entry->buffer;
}

// pass to pipeline — device-copy or staging upload
pipeline.run(ctx, cpu_data, stride, gpu_input);
```

Inside the pipeline's `run()`, use `cmd_upload_input()` which handles both paths:

```cpp
// handles device-to-device (gpu_input != null) or staging upload automatically
cmd_upload_input(staging_in_, input_buf_, frame_bytes, gpu_input);
```

#### Producing GPU-Resident Output

A coalesced plugin **emits resident-only output** — it acquires this frame's GPU buffer from the engine's K-deep edge ring, records compute into it, and publishes the handle. The engine owns the device→staging download and decides per-frame whether to record it, so the plugin no longer manages a staging copy or a `GpuOutputHandle`:

```cpp
// inside record_gpu, after acquiring an output frame `out` from the host pool.
// edge_ring_ctx is null only for legacy (non-coalesced) nodes — fall back to a
// self-contained CPU output there.
if (!rctx->edge_ring_ctx) { s->host.release_frame(out); return -1; }

// Acquire this frame's slot from the engine's K-deep output ring. The engine
// owns the device + staging buffers (one set per in-flight frame) and registers
// the staging against the returned gpu_handle, so its post-submit invalidate
// covers it. Pass the sizes/format the output needs; the ring is allocated
// lazily and re-allocated on a size change. At K=1 every call returns the same
// single slot.
SpcGpuEdgeBuffer ob = s->host.acquire_ringed_output(
    rctx->edge_ring_ctx, /*port*/ 0, w, h, out->stride, SPC_PIXEL_FORMAT_GRAY8,
    pipeline.output_device_size(), pipeline.output_staging_size());
if (!ob.device_buffer || ob.gpu_handle == 0) { s->host.release_frame(out); return -1; }

// Point the output binding at ob.device_buffer and record compute into the
// engine's secondary command buffer (the coalesced path).
// The engine hands back a non-null ob.staging_buffer ONLY when this edge has a
// CPU consumer (a downstream CPU node, preview, or subscription). Record the
// device→staging download just for that case — a pure GPU→GPU edge stays
// resident-only with no copy:
if (ob.staging_buffer) {
    barrier_compute_to_transfer();
    cmd_download_to_staging(static_cast<VkBuffer>(ob.device_buffer),
                            static_cast<VkBuffer>(ob.staging_buffer), out_bytes);
}

// Publish: point the output frame at the registry handle. No GpuOutputHandle,
// no manual lifecycle — the engine recycles the slot.
out->gpu_handle   = ob.gpu_handle;
out->gpu_flags   |= SPC_GPU_FLAG_RESIDENT;
out->frame_number = in->frame_number;
out->timestamp_ns = in->timestamp_ns;
rctx->outputs[0].type  = SPC_DATA_FRAME;
rctx->outputs[0].frame = out;
return 0;
```

`SpcGpuEdgeBuffer` carries `device_buffer` (the shader binding + transfer endpoint), `device_memory`, `staging_buffer`, the persistently-mapped `staging_mapped` pointer, `staging_size`, and the registry `gpu_handle` (outputs only — uploads return 0). Both accessors return a zeroed struct when the host predates the feature, so check `device_buffer` (and `gpu_handle` for outputs) and fall back to your CPU path. `s->host.has_edge_rings()` gates on support up front.

For a CPU-fed input on the coalesced path, acquire a per-frame upload slot the same way with `s->host.acquire_ringed_upload(rctx->edge_ring_ctx, in_port, device_size, staging_size)` — `memcpy` the CPU frame into `up.staging_mapped`, then record the upload from `up.staging_buffer` into `up.device_buffer`.

When a CPU consumer needs the data, the engine does an instant `memcpy` from the pre-populated staging — no extra GPU submission. After the coalesced submit it invalidates each materialized slot's staging so HOST_CACHED reads see fresh GPU writes.

> **Legacy / own-submit only:** `GpuOutputHandle` (`#include <gpu/gpu_output_handle.h>`) and its `bind_to_frame()` are retained for non-coalesced paths — one-shot init / warmup and `gpu_own_submit()` plugins that read back into their own pooled CPU frames. Coalesced plugins no longer use it.

#### Standard GPU Failure Contract

For the canonical "single input → single output, GPU with CPU fallback" pattern, use `GpuFailureTracker` to handle the device-lost cascade, transient-failure counter, and tear-down policy without writing the boilerplate by hand:

```cpp
#include <gpu/gpu_failure_tracker.h>

// in plugin state:
spc::gpu::GpuFailureTracker gpu_failure{"MyPlugin"};  // threshold defaults to 3

// on a successful GPU run:
s->gpu_failure.note_success();
return 0;

// on a failed GPU run (inside process(), after release_frame on the
// acquired output):
auto action = s->gpu_failure.on_failure(
    s->vk_ctx, s->vk_pipeline.last_vk_result(), &s->host.cached_log,
    [&] {
        s->gpu_available = false;
        if (s->vk_ctx) {
            s->gpu_output.release(s->vk_ctx.get());
            s->vk_pipeline.destroy(*s->vk_ctx);
            s->vk_ctx.reset();
        }
    });
if (action == spc::gpu::GpuFailureTracker::Action::DeviceLost)
    return SPC_ERR_DEVICE_LOST;
// fall through to CPU path
```

The tracker:
- probes `ctx->device_lost` and `pipeline.last_vk_result()`
- logs via the supplied `SpcLogContext` with human-readable `vk_result_str`
- counts consecutive failures, tears down at threshold (or immediately on `device_lost`)
- returns `Action::DeviceLost` when the engine should cascade (`SPC_ERR_DEVICE_LOST`) or `Action::Fallback` for the plugin's CPU path

**Plugins with unusual flows** (multi-output ports, mask inputs, multi-pass error handling, custom retry policy) can read `last_vk_result()` and `device_lost` directly and manage state themselves — `GpuFailureTracker` is the convenient default for the common case, not a required contract.

#### Respecting the GPU Toggle

The engine manages `gpu_enabled` (stop-only). Check `acquire_gpu_frame` in host services before entering the GPU path:

```cpp
#ifdef SPC_HAS_VULKAN
if (s->host.svc->acquire_gpu_frame && s->gpu_available) {
    // GPU path
} else
#endif
{
    // CPU fallback
}
```

When GPU is disabled, `acquire_gpu_frame` is NULL and the plugin falls through to CPU.

#### Handling GPU Failures

All GPU submission methods can fail. Always check return values and disable GPU on persistent failure:

```cpp
int result = process_gpu(s, in_frame, outputs);
if (result == 0) return 0;
// GPU failed — disable and fall through to CPU
s->gpu_available = false;
SPC_LOG_WARN(&s->host.cached_log, "GPU process failed, disabling GPU");
// ... CPU fallback ...
```

Inside GPU pipelines, `submit_and_wait()` / `submit_and_spin()` return `false` on:
- `VK_ERROR_DEVICE_LOST` — sets `ctx.device_lost` flag, all subsequent GPU calls bail immediately
- Queue submission failure
- Semaphore wait timeout (5s for spin, configurable for wait)

`GpuPipelineBase` checks `device_lost` at entry to `begin_recording()`, `submit_and_wait()`, `submit_and_spin()` — no wasted work after a GPU failure.

**Engine-side `record_gpu` failure handling.** The coalesced path adds a layer above plugin-level failure handling:

- `record_gpu` is invoked under SEH guard (`guarded_record_gpu`). A crash inside the plugin's recording is caught, the frame is canceled for the whole subgraph, and the offending plugin is demoted (single strike on SEH).
- A non-zero return from `record_gpu` is treated as a soft failure. After `kRecordGpuFailThreshold` (= 3) consecutive soft failures the engine demotes the plugin out of the subgraph.
- **Demotion** clears `ctx->subgraph_executor` and `ctx->gpu_active` for the offending member; subsequent iterations route through the plugin's `process()` (CPU fallback). Sibling members in the subgraph keep running.
- The return value contract for `record_gpu`: `0` = success, non-zero = "fail this frame for the whole subgraph". For permanent device failure, set `ctx.device_lost` so the watchdog escalates to a profile rebuild.
- **`SPC_NO_DATA` exception**: "no frame this tick" (live-stream stall, exposure still in flight, reconnect backoff window) cancels the frame but does **not** count toward the demote, and in `process()` it skips the error-backoff entirely (the engine retries after a short fixed poll, `kNoDataPollInterval`). Return it for transient input droughts so a hiccup never permanently costs the GPU path; return a real error for anything that should back off, demote, or reach the watchdog.

#### CMake Setup

```cmake
spc_add_plugin(my_filter SOURCES my_filter_plugin.cpp)
target_link_libraries(my_filter PRIVATE SpeculorSDK::spclib)

if(SPC_VULKAN_FOUND AND SPC_GLSLANG_VALIDATOR)
    spc_enable_gpu(my_filter)
    spc_add_gpu_shaders(my_filter SHADERS my_compute.comp)
    target_sources(my_filter PRIVATE my_gpu_pipeline.h my_gpu_pipeline.cpp)
endif()
```

`SPC_VULKAN_FOUND` and `SPC_GLSLANG_VALIDATOR` are set by `find_package(SpeculorSDK)` — the bundle ships the Vulkan loader/headers and `glslangValidator`, so a GPU plugin needs no separately installed Vulkan SDK.

---

## Two-Phase Shutdown

Pipeline shutdown is a two-phase protocol because plugins do two different things on stop, and they need to happen at different times relative to the per-node worker thread:

| Phase | Vtable slot | When the engine calls it | What the plugin must do |
|------|-------------|---------------------------|--------------------------|
| 1 — abort signal | `request_stop` (optional) | **Before** `wait_for_process_drain` — concurrently with an in-flight `process()` / `record_gpu` | Set atomic flags only. Kick interrupt callbacks (FFmpeg `AVIOInterruptCB`, `shutdown(socket)`). Return immediately. **Do not free anything the worker is still reading.** |
| 2 — teardown | `stop` (optional) | **After** `wait_for_process_drain` — the worker is no longer inside `process()` / `record_gpu` | Free resources, close devices, reset shared_ptrs. Free to do destructive work — the worker is out. |

The split exists because folding both into `stop()` would force the teardown half to race with concurrent `process()`/`record_gpu` calls. For plugins that hold an FFmpeg `AVFormatContext`, freeing the context (`avformat_close_input`) while another thread is mid-`h264_parse` is undefined behavior and crashes hard. The two-phase protocol mirrors the standard cancel+join pattern: signal first, destroy after.

### When to implement `request_stop`

You need it when your `process()` / `record_gpu` can block on something that *only an external poke can unblock*:

- FFmpeg `av_read_frame` on an RTSP/HTTP source with a stalled connection.
- Blocking `recv()` on a TCP socket waiting for a peer that never sends.
- Long `WaitForSingleObject` / `pthread_cond_wait` on something the engine's `stop_token` doesn't reach.

You don't need it for plugins whose worker is naturally bounded — pure compute, queue-driven processing, fast device reads. The engine's `stop_token` and queue shutdown alone are enough; leave the slot null.

If a plugin that needs `request_stop` doesn't have it, `wait_for_process_drain` will time out (3s), the worker may still be wedged when teardown runs, and you risk SEH crashes on freed state.

### Example — an RTSP video source

```cpp
static int request_stop(SpcPluginInstance* inst) {
    auto* s = state(inst);
    s->stop_requested.store(true, std::memory_order_release);
    if (s->reader) s->reader->request_stop();   // kicks FFmpeg AVIOInterruptCB
    return 0;
}

static int stop(SpcPluginInstance* inst) {
    auto* s = state(inst);
    // Phase 2 — teardown. Worker is guaranteed to be out of process()/record_gpu.
    s->reader.reset();        // free demuxer, hwaccel state
    if (s->vk_ctx) { s->pipeline.destroy(*s->vk_ctx); s->vk_ctx.reset(); }
    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor     = get_descriptor,
    .create_instance    = create_instance,
    .destroy_instance   = destroy_instance,
    .set_parameter      = set_parameter,
    .get_parameter      = get_parameter,
    .process            = process,
    .start              = start,
    .stop               = stop,
    .set_host_services  = set_host_services,
    .validate_mandatory = validate_mandatory,
    .record_gpu         = record_gpu,
    .request_stop       = request_stop
)
```

Add `.validate_mandatory`, `.request_stop`, and `.record_gpu` only if you need them — `SPC_PLUGIN_VTABLE` defaults every unmentioned slot to `nullptr`.

---

## Working with Tables

Tables are schema-described binary flat arrays, designed for the hot path (detections, tracks, sensor readings).

### Declaring the Schema

Declare table schemas in your descriptor's port definitions:

```cpp
.output_table("detections", "Detections", {
    {"x", SPC_FIELD_FLOAT},
    {"y", SPC_FIELD_FLOAT},
    {"w", SPC_FIELD_FLOAT},
    {"h", SPC_FIELD_FLOAT},
    {"confidence", SPC_FIELD_FLOAT},
})
```

Available field types: `SPC_FIELD_FLOAT`, `SPC_FIELD_INT32`, `SPC_FIELD_UINT32`, `SPC_FIELD_BOOL`, `SPC_FIELD_INT8`, `SPC_FIELD_UINT8`, `SPC_FIELD_INT16`, `SPC_FIELD_UINT16`, `SPC_FIELD_FLOAT64`, `SPC_FIELD_INT64`, `SPC_FIELD_UINT64`, `SPC_FIELD_STRING32`, `SPC_FIELD_STRING64`, `SPC_FIELD_STRING128`, `SPC_FIELD_STRING256`, `SPC_FIELD_DECIMAL`.

#### Decimal Fields

Use `SPC_FIELD_DECIMAL` for exact financial/monetary values that cannot tolerate float rounding. The `SpcDecimal` struct is 16 bytes: `int64_t mantissa` + `int16_t scale` + padding. Value = mantissa / 10^scale.

```cpp
// table output with decimal field
.output_table("out", "Output", {
    {"symbol", SPC_FIELD_STRING32},
    {"price", SPC_FIELD_DECIMAL},
    {"volume", SPC_FIELD_INT64},
})

// write/read decimal fields
SpcDecimal price = spc::spc_decimal_make(19999, 2);  // 199.99
spc_table_set_decimal(&table, row, price_offset, price);
SpcDecimal got = spc_table_get_decimal(&table, row, price_offset);

// format for display/JSON (always as string, never as JSON number)
char buf[32];
spc::spc_decimal_to_string(price, buf, sizeof(buf));  // "199.99"

// decimal parameter
.decimal_param("price", "Price",
    spc::spc_decimal_make(0, 2),       // min
    spc::spc_decimal_make(9999999, 2), // max
    spc::spc_decimal_make(0, 2),       // default
    spc::spc_decimal_make(1, 2))       // step (0.01)
```

### Producing Table Data

```cpp
// in your state struct
SpcTable output_table{};
uint32_t offsets[5]{};    // one per field
uint32_t stride{};

// in create_instance — precompute offsets from the schema
auto* desc = get_descriptor();
spc_schema_compute_offsets(&desc->ports[0].schema, s->offsets, &s->stride);
spc_table_init(&s->output_table, s->stride, &desc->ports[0].schema);

// in process — fill the table
spc_table_resize(&s->output_table, detection_count);
for (uint32_t i = 0; i < detection_count; ++i) {
    spc_table_set_float(&s->output_table, i, s->offsets[0], detections[i].x);
    spc_table_set_float(&s->output_table, i, s->offsets[1], detections[i].y);
    spc_table_set_float(&s->output_table, i, s->offsets[2], detections[i].w);
    spc_table_set_float(&s->output_table, i, s->offsets[3], detections[i].h);
    spc_table_set_float(&s->output_table, i, s->offsets[4], detections[i].conf);
}
spc::set_table_output(outputs[0], &s->output_table);

// in destroy_instance — free the table
spc_table_free(&s->output_table);
```

### Consuming Table Data

```cpp
const auto* table = spc::input_table(inputs, input_count, 0);
if (!table || table->record_count == 0) return 0;

// resolve field offsets by name (do this once, cache in state)
uint32_t off_x = spc_schema_field_offset(table->schema, "x");
uint32_t off_y = spc_schema_field_offset(table->schema, "y");

for (uint32_t i = 0; i < table->record_count; ++i) {
    float x = spc_table_get_float(table, i, off_x);
    float y = spc_table_get_float(table, i, off_y);
    // ...
}
```

### Table Lifecycle Functions

| Function | Purpose |
|----------|---------|
| `spc_table_init(table, stride, schema)` | Initialize a table with a given stride and schema pointer |
| `spc_table_resize(table, count)` | Set the record count; allocates/reallocates if needed. Returns 0 on success. |
| `spc_table_clear(table)` | Reset record count to 0 without freeing memory |
| `spc_table_free(table)` | Free the data buffer and reset all fields |

---

## Signal Ports

Signals are direct-call connections for high-throughput data (audio samples, SDR IQ data). Unlike table ports where the engine copies data through queues, signal data is passed by pointer directly from the producer's `process()` thread to the consumer's `on_signal()` callback.

### When to Use Signals vs Tables

- **Tables**: Engine copies data through queues. Safe for different-rate producers/consumers. Use for detection results, track lists, metadata.
- **Signals**: Zero-copy direct call. Producer and consumer must run at compatible rates. Use for audio, SDR, high-frequency sensor data.

### Declaring Signal Ports

```cpp
// mono producer
.output_signal("samples_out", "Samples", {
    {"amplitude", SPC_FIELD_FLOAT},
})
.streaming()

// mono consumer
.input_signal("samples_in", "Samples", {
    {"amplitude", SPC_FIELD_FLOAT},
})

// stereo producer (convention: amplitude=left, right=right)
.output_signal("audio_out", "Audio Out", {
    {"amplitude", SPC_FIELD_FLOAT},
    {"right", SPC_FIELD_FLOAT},
})

// stereo consumer
.input_signal("audio_in", "Audio In", {
    {"amplitude", SPC_FIELD_FLOAT},
    {"right", SPC_FIELD_FLOAT},
})
```

Mono consumers (`{amplitude}`) can connect to stereo outputs via structural subtyping — they receive only the left channel.

Declaring an `input_signal` automatically sets `SPC_CAP_SIGNAL_INPUT`.

### Implementing `on_signal`

The consumer implements an `on_signal` handler, typically using `SpcRingBuffer` to decouple the producer's thread from the consumer's process thread:

```cpp
// in state struct
SpcRingBuffer* ring = nullptr;

// in create_instance
s->ring = spc_ring_create(8192, stride);

// signal handler — called from producer's thread
static int handle_signal(MyState* s, uint32_t port_index, const SpcTable* data) {
    if (!data || !data->data) return -1;
    spc_ring_write(s->ring, data->data, data->record_count);
    return 0;
}
SPC_PLUGIN_SIGNAL_CALLBACK(MyState, handle_signal)

// in process — drain the ring buffer on the consumer's thread
uint32_t available = spc_ring_available(s->ring);
// ... read and process
```

### Thread Safety Contract

- `on_signal` is called from the **producer's thread**, not the consumer's process thread.
- The `data` pointer is only valid for the duration of the `on_signal` call.
- Use `SpcRingBuffer` (lock-free SPSC) to safely transfer data between threads.
- Do not do heavy work inside `on_signal` — copy the data and return.

---

## Logging

Plugins log through the host's logging system using the `SPC_LOG_*` macros. These require a pointer to an `SpcLogContext`, which is automatically populated when you use `SPC_PLUGIN` or `SPC_PLUGIN_HOST_SERVICES`:

```cpp
auto* s = state(inst);
SPC_LOG_INFO(&s->host.cached_log, "Processing frame %llu at %dx%d",
             static_cast<unsigned long long>(frame->frame_number),
             frame->width, frame->height);

SPC_LOG_ERROR(&s->host.cached_log, "Failed to open device: %s", error_msg);
SPC_LOG_WARN(&s->host.cached_log, "Buffer overflow, dropping %u samples", dropped);
SPC_LOG_DEBUG(&s->host.cached_log, "Phase: %.4f", phase);
```

The macros use `printf`-style formatting. Log levels:

| Macro | Level | Use Case |
|-------|-------|----------|
| `SPC_LOG_DEBUG` | Debug | Detailed diagnostic info, disabled in release |
| `SPC_LOG_INFO` | Info | Normal operational messages |
| `SPC_LOG_WARN` | Warning | Recoverable issues |
| `SPC_LOG_ERROR` | Error | Failures that affect plugin operation |

If host services are not yet injected (or the host does not provide logging), the macros are safe to call — they silently do nothing.

---

## Exporting the VTable

Use `SPC_PLUGIN_VTABLE(...)` to emit the `spc_plugin_vtable` symbol. It uses C++20 designated initializers: list only the slots your plugin implements, and the rest default to `nullptr`. Field order must match `SpcPluginVTable`'s declaration order in `plugin_api.h`.

The full slot list, in declaration order:

| Slot | When to set |
|---|---|
| `get_descriptor` | Always — required |
| `create_instance` | Always — required |
| `destroy_instance` | Always — required |
| `set_parameter` | Always — wire to your auto-params or hand-written setter |
| `get_parameter` | Always — wire to your auto-params or hand-written getter |
| `process` | Always — required |
| `start` | Streaming sources that need lifecycle |
| `stop` | Pairs with `start` |
| `set_host_services` | If plugin uses logging or frame allocation |
| `on_signal` | Plugin consumes `SPC_DATA_SIGNAL` input ports |
| `on_input_event` | Interactive plugin (mouse/keyboard) |
| `scan_devices` | Plugin enumerates hardware (e.g. SDR, cameras) |
| `on_control_response` | Plugin receives control-channel replies |
| `get_list_rows` | Plugin has list parameters |
| `set_list_rows` | Pairs with `get_list_rows` |
| `validate_mandatory` | Plugin has *conditional* mandatory params; omit for declarative `.mandatory()` (engine runs default check) |
| `record_gpu` | Plugin opts into engine-driven coalesced GPU submit |
| `request_stop` | Plugin's `process()`/`record_gpu` can block on uninterruptible I/O |

### Most common shapes

**CPU filter** — no lifecycle, no GPU, no event handling:

```cpp
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

**Streaming source** — camera, network feed, file reader:

```cpp
SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services
)
```

**Coalesced GPU filter** with start/stop (the common GPU plugin shape — eager init in `start()` is required because `record_gpu` bypasses `process()`):

```cpp
SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services,
    .record_gpu        = record_gpu
)
```

**GPU source with blocking I/O and conditional mandatory params** (the typical network video-source shape):

```cpp
SPC_PLUGIN_VTABLE(
    .get_descriptor     = get_descriptor,
    .create_instance    = create_instance,
    .destroy_instance   = destroy_instance,
    .set_parameter      = set_parameter,
    .get_parameter      = get_parameter,
    .process            = process,
    .start              = start,
    .stop               = stop,
    .set_host_services  = set_host_services,
    .validate_mandatory = validate_mandatory,
    .record_gpu         = record_gpu,
    .request_stop       = request_stop
)
```

### Conditional slots

Preprocessor directives (`#ifdef`, `#if`) cannot appear inside the macro arguments. To conditionally include a slot, route it through a helper macro that resolves to either `.slot = fn,` or an empty token sequence:

```cpp
#ifdef SPC_HAS_VULKAN
  #define MY_RECORD_GPU_SLOT .record_gpu = record_gpu,
#else
  #define MY_RECORD_GPU_SLOT
#endif

SPC_PLUGIN_VTABLE(
    ...
    .stop              = stop,
    MY_RECORD_GPU_SLOT
    .request_stop      = request_stop
)
```

Slot semantics for the optional trailing slots:

- **`on_input_event`** — GUI interaction handler (mouse/keyboard events for interactive plugins)
- **`scan_devices`** — hardware device enumeration (called before instance creation)
- **`on_control_response`** — control channel response handler (LIST/GET replies from upstream nodes, delivered independently from `process()`)
- **`validate_mandatory`** — conditional mandatory-parameter check; null = host runs default flag-based check
- **`record_gpu`** — coalesced GPU recording (see [GPU Compute Plugins](#gpu-compute-plugins))
- **`request_stop`** — pre-drain abort signal (see [Two-Phase Shutdown](#two-phase-shutdown))

### Control Response Callback

Plugins that send control commands (LIST/GET) and need to process the response implement `on_control_response`:

```cpp
// handler signature: void handler(StateType*, const SpcControlMsg*)
static void on_ctrl_response(MyState* s, const SpcControlMsg* response) {
    if (response->msg_type != SPC_CTRL_RESPONSE) return;
    // process response parameters...
}
SPC_PLUGIN_CONTROL_RESPONSE(MyState, on_ctrl_response)
```

The response callback is called on the node's own execution thread, separate from `process()`. Control connections are independent from data inputs — they do not affect `input_count` or the process loop.

---

## CMake Integration

### Project Setup

`find_package(SpeculorSDK REQUIRED)` resolves from the extracted bundle and brings in the `SpeculorSDK::` imported targets plus the `spc_add_plugin()` helper (from the bundled `PluginHelpers.cmake`). A complete plugin project is two files:

**`CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_plugin LANGUAGES CXX)

find_package(SpeculorSDK REQUIRED)
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

Configure with `CMAKE_PREFIX_PATH` pointing at the extracted bundle:

**Linux**
```bash
cmake -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$HOME/speculor-sdk/SpeculorSDK-<version>-Linux-x86_64"
cmake --build build
```

**Windows** (Developer Command Prompt)
```powershell
cmake -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="C:\path\to\SpeculorSDK-<version>-Windows-x64"
cmake --build build
```

### `spc_add_plugin()`

```cmake
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

This creates a shared library target with the correct settings:
- Adds the SDK include directories (`include/speculor/` and the shared plugin headers in `include/speculor_common/`)
- Links against `SpeculorSDK::speculor_sdk`
- Defines `SPC_PLUGIN_EXPORT` for proper symbol visibility
- Removes the `lib` prefix (plugins are loaded by name)
- Sets hidden default visibility, so only the vtable symbol is exported
- Configures RPATH for runtime dependency resolution against the bundled libraries
- Applies the SDK's compiler warning set

The build output is a loadable plugin shared library (`my_plugin.dll` / `my_plugin.so`). Drop it into the host application's plugin directory to load it.

### Linking Additional Libraries

The bundle's dependencies are available as imported targets, and you can link your own on top:

```cmake
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
target_link_libraries(my_plugin PRIVATE SpeculorSDK::spclib)
```

---

## Complete Example: A Streaming Counter Source

This example shows a more complex plugin that generates frames with a counter overlay. It demonstrates custom create/destroy, side effects in `set_parameter`, start/stop lifecycle, frame allocation with pool fallback, and logging. It is a generator, so it carries `.data_source()` — its output originates outside the pipeline's dataflow, which is what makes it recordable and replayable.

```cpp
#include <speculor/plugin_helpers.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

struct CounterState {
    spc::HostServices host;

    // parameters
    int32_t width = 320;
    int32_t height = 240;
    float target_fps = 10.0f;

    // runtime
    uint64_t frame_count = 0;
    std::chrono::steady_clock::time_point last_frame_time;

    // fallback buffer (used when pool frame not available)
    std::vector<uint8_t> buffer;
    SpcFrame local_frame{};
};

SPC_PLUGIN_CAST(CounterState)
SPC_PLUGIN_HOST_SERVICES(CounterState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("counter_source", "Counter Source", "Generators")
        .author("Speculor").version("0.1.0")
        .description("Generates frames with a counter value rendered as brightness")
        .output("image_out", "Image", SPC_DATA_FRAME)
        .int_param("width", "Width", 64, 1920, 320, 1, "Resolution")
        .int_param("height", "Height", 64, 1080, 240, 1, "Resolution")
        .float_param("target_fps", "Target FPS", 1.0f, 120.0f, 10.0f, 1.0f, "Timing")
        .streaming()
        .frame_alloc()
        .data_source()
)

// --- lifecycle ---------------------------------------------------------------

static SpcPluginInstance* create_instance() {
    auto* s = new CounterState{};
    s->buffer.resize(static_cast<size_t>(s->width) * s->height);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void destroy_instance(SpcPluginInstance* inst) {
    delete state(inst);
}

// --- parameters (with side effects on resize) --------------------------------

static int set_parameter(SpcPluginInstance* inst, const char* name,
                          const SpcParameterDesc* value) {
    auto* s = state(inst);
    if (spc::try_set_float(name, value, "target_fps", s->target_fps)) return 0;

    int32_t w = s->width, h = s->height;
    bool resized = spc::try_set_int(name, value, "width", w)
                || spc::try_set_int(name, value, "height", h);
    if (!resized) return -1;
    s->width = w;
    s->height = h;
    s->buffer.resize(static_cast<size_t>(s->width) * s->height);
    SPC_LOG_INFO(&s->host.cached_log, "Resized to %dx%d", s->width, s->height);
    return 0;
}

static int get_parameter(SpcPluginInstance* inst, const char* name,
                          SpcParameterDesc* out) {
    auto* s = state(inst);
    if (spc::try_get_int(name, out, "width", s->width)) return 0;
    if (spc::try_get_int(name, out, "height", s->height)) return 0;
    if (spc::try_get_float(name, out, "target_fps", s->target_fps)) return 0;
    return -1;
}

// --- streaming ---------------------------------------------------------------

static int start(SpcPluginInstance* inst) {
    auto* s = state(inst);
    s->frame_count = 0;
    s->last_frame_time = std::chrono::steady_clock::now();
    SPC_LOG_INFO(&s->host.cached_log, "Counter source started (%dx%d @ %.0f fps)",
                 s->width, s->height, s->target_fps);
    return 0;
}

static int stop(SpcPluginInstance* inst) {
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "Counter source stopped (%llu frames)",
                 static_cast<unsigned long long>(s->frame_count));
    return 0;
}

// --- process -----------------------------------------------------------------

static int process(SpcPluginInstance* inst, const SpcData* /*inputs*/,
                   uint32_t /*input_count*/, SpcData* outputs,
                   uint32_t output_count) {
    auto* s = state(inst);
    if (output_count < 1) return -1;

    // pace to target FPS
    if (s->target_fps > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / s->target_fps));
        auto next = s->last_frame_time + interval;
        if (now < next) std::this_thread::sleep_until(next);
        s->last_frame_time = std::chrono::steady_clock::now();
    }

    auto w = static_cast<uint32_t>(s->width);
    auto h = static_cast<uint32_t>(s->height);

    // try pool allocation first
    SpcFrame* out = s->host.acquire_frame(0, w, h, SPC_PIXEL_FORMAT_GRAY8);

    uint8_t* dst = out ? out->data : s->buffer.data();
    uint32_t stride = out ? out->stride : w;

    // render: fill with brightness based on counter
    uint8_t brightness = static_cast<uint8_t>(s->frame_count % 256);
    for (uint32_t y = 0; y < h; ++y) {
        std::memset(dst + y * stride, brightness, w);
    }

    if (out) {
        out->frame_number = s->frame_count++;
        spc::set_frame_output(outputs[0], out);
    } else {
        s->local_frame.data = s->buffer.data();
        s->local_frame.width = w;
        s->local_frame.height = h;
        s->local_frame.stride = w;
        s->local_frame.format = SPC_PIXEL_FORMAT_GRAY8;
        s->local_frame.frame_number = s->frame_count++;
        spc::set_frame_output(outputs[0], &s->local_frame);
    }

    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services
)
```

---

## Shared Plugin Utilities

The bundle ships a set of header-only plugin-side utilities in `include/speculor_common/`. `spc_add_plugin()` puts that directory on your include path, so you include them unprefixed (e.g. `#include <cv_helpers.h>`, `#include <gpu/gpu_utils.h>`):

| Header | Purpose |
|--------|---------|
| `spc_clock.h` | Shared-clock accessors (`spc::clock::now_utc_ns` / `now_mono_ns` / `discipline`) — see [Timestamps and the shared clock](#timestamps-and-the-shared-clock) |
| `spc_ui_panel.h` | Immediate-mode UI widget system (`spc::ui::Panel`) for rendering interactive control panels directly on video frames. Provides buttons, toggles, spinners, sliders, dropdowns, frequency displays, and grouped layouts — the basis for on-frame control surfaces such as an SDR tuning interface |
| `spc_ui_theme.h` | Catppuccin Mocha color theme (RGB order) and sizing constants for the UI panel system |
| `spc_ui_hit.h` | Hit-testing infrastructure for UI panel mouse interaction — fixed-capacity `HitRegionSet` with actions like `Toggle`, `EnumOption`, `SpinnerInc/Dec`, `SliderTrack`, `FreqDisplay` |
| `cv_helpers.h` | OpenCV bridge: `frame_to_mat()` / `mat_to_frame()` zero-copy wrappers, `cv_type_for_format()` pixel format conversion, `copy_frame_rows()` stride-aware copy |
| `recording_helpers.h` | Shared recording utilities: `date_folder()` and `timestamp_filename()` for file path generation |
| `metadata_helpers.h` | `parse_metadata_for_container()` — converts JSON metadata into MP4/MOV container tags (ISO 6709 location, creation_time, artist/comment remapping) |
| `table_serialize.h` | `serialize_table()` — converts `SpcTable` rows to `nlohmann::json` array using schema field types |
| `pocketfft_hdronly.h` | Vendored single-header FFT library for spectrum analysis plugins |
| `gpu/…` | Vulkan GPU helpers — see [GPU Acceleration](#gpu-acceleration-vulkan-compute) |

### SDR Parameter Standard (`speculor/sdr_params.h`)

Standardized parameter names for SDR plugins, organized into groups:

| Group | Parameters |
|-------|-----------|
| **Tuning** | `center_freq`, `sample_rate`, `bandwidth`, `ddc1_type` |
| **Demodulator** | `demod_mode`, `demod_bw`, `demod_freq_offset`, `demod_filter_shift`, `demod_filter_length` |
| **Gain** | `agc_enabled`, `agc_speed`, `mgc_gain`, `max_agc_gain`, `attenuator`, `preamplifier` |
| **Filters** | `squelch_level`, `notch_enabled/freq/bw/length`, `noise_blanker`, `adc_nb_threshold` |
| **Hardware** | `dithering`, `ext_reference`, `inverted`, `channel`, `mw_filter`, `led_mode`, `direct_sampling`, `bias_tee`, `freq_correction` |

The SDR Control Panel plugin discovers available parameters dynamically via the parameter list — SDR source plugins only declare what their hardware supports and the control panel adapts automatically.

---

## Best Practices

### ABI Safety

- Never pass C++ types (`std::string`, `std::vector`, classes with vtables) across the plugin boundary. The ABI uses only POD types and fixed-size buffers.
- Use `extern "C"` linkage for all exported symbols. The `SPC_PLUGIN_VTABLE` macro handles this.
- Do not store pointers to host memory beyond the lifetime of the call that provided them (except `SpcHostServices`, which is valid for the plugin instance lifetime).

### Memory Management

- Free everything you allocate. Call `spc_table_free()` in `destroy_instance()` for any tables initialized with `spc_table_init()`.
- Release acquired frames that you decide not to use as output: `s->host.release_frame(frame)`.
- Use RAII containers (`std::vector`, `std::unique_ptr`) inside your state struct. The `delete` in `destroy_instance()` will call the destructor.

### Performance

- Precompute field offsets in `create_instance()`, not in `process()`. The `spc_schema_compute_offsets()` call is not free.
- Use pool frames (`.frame_alloc()`) to avoid a copy on the output path.
- Minimize allocations in `process()`. Pre-allocate buffers in `create_instance()` or resize them lazily.
- For signal producers, batch samples (64-8192 per call) rather than calling `on_signal` per sample.

### Thread Safety

- `process()` is called from the engine's worker thread. It is safe to access your state struct without locks — the engine guarantees single-threaded access to each instance's `process()`.
- `set_parameter()` and `get_parameter()` are called from the UI thread (and from other nodes' threads via the control channel) **concurrently** with `process()`. Writing plain state fields from `set_parameter` while `process()` reads them is a data race — undefined behavior even for `int32_t`/`float`. Keep every parameter field that `process()` reads in one POD block held in `spc::SharedParams<T>` (`param_dispatch.h`): `set_parameter` mutates it via `update()` with the usual `try_set_*` helpers, `process()` copies it once per frame via `snapshot()`. Snapshotting once per frame also guarantees all reads within the frame agree (no check/use gaps between a count loop and a fill loop).
- Never call into stateful processing objects (detectors, trackers, prepared statements, encoders) from `set_parameter` — they're owned by the worker. Set a `std::atomic<bool>` dirty flag and apply the snapshotted parameters from `process()`.
- `on_signal()` is called from the producer's thread. Use `SpcRingBuffer` (lock-free SPSC) to decouple it from `process()`.

### Error Handling

- Return `0` for success, `-1` for failure from `process()`, `set_parameter()`, `get_parameter()`, `start()`, `stop()`, and `on_signal()`.
- Check `input_count` and `output_count` before accessing arrays.
- Use the `spc::input_*` helpers — they return `nullptr` or a default value when the index is out of range or the type does not match.
- Log errors with `SPC_LOG_ERROR` to help users diagnose pipeline issues.

### Plugin Health & the Watchdog

The engine runs a **NodeWatchdog** thread that monitors every node for hangs and crashes. Plugin authors should be aware of the following:

- **Hang detection**: If `process()` does not return within the configured timeout (default 10s for filters, 30s for sources and GPU plugins), the engine considers the node hung. It will automatically abandon the stuck thread, create a fresh plugin instance, and restart the node. This means your plugin may be re-instantiated (`create_instance` → `set_host_services` → `start` → `process`) at any time during pipeline execution.
- **Crash detection**: If `process()` crashes (access violation on Windows, segfault on POSIX), the engine catches the exception, marks the node as crashed, and performs the same restart sequence.
- **Restart limit**: After a configurable number of restart attempts (default 3), the node is permanently disabled and downstream nodes are cascade-disabled. The user sees a health event in the UI.
- **Design for restartability**: Avoid storing state that cannot be reconstructed from parameters alone. If your plugin initializes hardware (cameras, serial ports), ensure `start()` can open the device cleanly even if a previous instance was abandoned.
- **Avoid blocking indefinitely in `process()`**: Use timeouts on I/O operations (socket reads, device reads). If you must wait, check periodically and return `SPC_NO_DATA` when the wait is a normal input drought (the engine retries after a short fixed poll with no error state), or an error code for genuine failures (the engine's backoff mechanism retries with exponential delay).
- **No global mutable state**: If two instances of your plugin exist simultaneously (the old hung instance on a detached thread, plus the new replacement), they must not corrupt each other via shared globals. Use per-instance state exclusively.

In short, the watchdog treats every node as replaceable: it tracks per-node liveness, and on a hang or crash it abandons the instance, builds a fresh one through the normal lifecycle, and counts the restart against a limit before disabling the node and cascading downstream. Nothing about that sequence is visible to a correctly written plugin beyond the requirement that a fresh instance must be able to reach a working state from its parameters alone.

---

## SDK Reference

### Limits

| Constant | Value | Meaning |
|----------|-------|---------|
| `SPC_PORT_MAX` | 16 | Maximum ports per plugin |
| `SPC_PLUGIN_PARAM_MAX` | 64 | Maximum parameters per plugin |
| `SPC_PARAM_NAME_MAX` | 64 | Maximum parameter/port name length |
| `SPC_PARAM_STRING_MAX` | 256 | Maximum string parameter value length |
| `SPC_PARAM_TEXT_MAX` | 4096 | Maximum multi-line text parameter length |
| `SPC_PARAM_ENUM_MAX` | 32 | Maximum enum options per parameter |
| `SPC_PARAM_ENUM_LABEL_MAX` | 32 | Maximum enum label length |
| `SPC_SCHEMA_MAX_FIELDS` | 48 | Maximum fields per table/signal schema |
| `SPC_FIELD_NAME_MAX` | 64 | Maximum field name length |
| `SPC_CONTROL_MSG_MAX` | 64 | Maximum parameter entries per control message (= `SPC_PLUGIN_PARAM_MAX`) |
| `SPC_LIST_COL_MAX` / `SPC_LIST_ROW_MAX` | 8 / 256 | Maximum columns / rows per list parameter |

### Process Input/Output Helpers

```cpp
// input extraction (return nullptr/default if index out of range or type mismatch)
const SpcFrame*      spc::input_frame(const SpcData* inputs, uint32_t count, uint32_t index);
const SpcTable*      spc::input_table(const SpcData* inputs, uint32_t count, uint32_t index);
const SpcRecord*     spc::input_record(const SpcData* inputs, uint32_t count, uint32_t index);
const SpcPacket*     spc::input_packet(const SpcData* inputs, uint32_t count, uint32_t index);
float                spc::input_scalar(const SpcData* inputs, uint32_t count, uint32_t index,
                                       float default_val = 0.0f);
bool                 spc::input_scalar_bool(const SpcData* inputs, uint32_t count, uint32_t index,
                                            bool default_val = false);
SpcScalar            spc::input_scalar_typed(const SpcData* inputs, uint32_t count, uint32_t index);
const SpcControlMsg* spc::input_control_msg(const SpcData* inputs, uint32_t count, uint32_t index);

// output assignment (set_scalar_output is overloaded for float/double/int32/uint32/
// int64/uint64/SpcScalar; set_scalar_output_bool for bools)
void spc::set_frame_output(SpcData& out, SpcFrame* frame);
void spc::set_table_output(SpcData& out, SpcTable* table);
void spc::set_scalar_output(SpcData& out, float value);
void spc::set_record_output(SpcData& out, SpcRecord* record);
void spc::set_packet_output(SpcData& out, SpcPacket* packet);
```

`SpcControlMsg` supersedes the older `SpcParamCmd` name. The old spellings remain as aliases (`spc::input_param_cmd`, `using SpcParamCmd = SpcControlMsg`) so existing plugin sources keep compiling — prefer the `control_msg` names in new code.

### Table Helper Functions

```c
// schema inspection
void     spc_schema_compute_offsets(const SpcPortSchema* schema,
                                    uint32_t* offsets_out, uint32_t* stride_out);
uint32_t spc_field_byte_size(SpcFieldType type);
uint32_t spc_schema_field_offset(const SpcPortSchema* schema, const char* field_name);

// lifecycle
void spc_table_init(SpcTable* table, uint32_t stride, const SpcPortSchema* schema);
int  spc_table_resize(SpcTable* table, uint32_t record_count);
void spc_table_clear(SpcTable* table);
void spc_table_free(SpcTable* table);

// typed accessors (field_offset from spc_schema_compute_offsets or spc_schema_field_offset)
float    spc_table_get_float(const SpcTable*, uint32_t record, uint32_t field_offset);
int32_t  spc_table_get_int32(const SpcTable*, uint32_t record, uint32_t field_offset);
uint32_t spc_table_get_uint32(const SpcTable*, uint32_t record, uint32_t field_offset);
int32_t  spc_table_get_bool(const SpcTable*, uint32_t record, uint32_t field_offset);
int8_t   spc_table_get_int8(const SpcTable*, uint32_t record, uint32_t field_offset);
uint8_t  spc_table_get_uint8(const SpcTable*, uint32_t record, uint32_t field_offset);
int16_t  spc_table_get_int16(const SpcTable*, uint32_t record, uint32_t field_offset);
uint16_t spc_table_get_uint16(const SpcTable*, uint32_t record, uint32_t field_offset);
double   spc_table_get_float64(const SpcTable*, uint32_t record, uint32_t field_offset);

void spc_table_set_float(SpcTable*, uint32_t record, uint32_t field_offset, float value);
void spc_table_set_int32(SpcTable*, uint32_t record, uint32_t field_offset, int32_t value);
void spc_table_set_uint32(SpcTable*, uint32_t record, uint32_t field_offset, uint32_t value);
void spc_table_set_bool(SpcTable*, uint32_t record, uint32_t field_offset, int32_t value);
void spc_table_set_int8(SpcTable*, uint32_t record, uint32_t field_offset, int8_t value);
void spc_table_set_uint8(SpcTable*, uint32_t record, uint32_t field_offset, uint8_t value);
void spc_table_set_int16(SpcTable*, uint32_t record, uint32_t field_offset, int16_t value);
void spc_table_set_uint16(SpcTable*, uint32_t record, uint32_t field_offset, uint16_t value);
void spc_table_set_float64(SpcTable*, uint32_t record, uint32_t field_offset, double value);
```

### Ring Buffer Functions

```c
SpcRingBuffer* spc_ring_create(uint32_t capacity, uint32_t element_size);
void           spc_ring_destroy(SpcRingBuffer* rb);
void           spc_ring_reset(SpcRingBuffer* rb);
uint32_t       spc_ring_write(SpcRingBuffer* rb, const void* src, uint32_t count);
uint32_t       spc_ring_read(SpcRingBuffer* rb, void* dst, uint32_t count);
uint32_t       spc_ring_peek(const SpcRingBuffer* rb, void* dst, uint32_t count);
uint32_t       spc_ring_available(const SpcRingBuffer* rb);
uint32_t       spc_ring_free(const SpcRingBuffer* rb);
```

### Color Helpers

```cpp
void     spc::color_unpack(uint32_t rgba, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);
uint32_t spc::color_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
```

### SDK Version

```cpp
std::string ver = spc::format_sdk_version(SPC_SDK_VERSION);  // e.g. "0.22.0"
```

`version.h` also exposes `SPC_SDK_VERSION_MAJOR` / `_MINOR` / `_PATCH` for compile-time feature gating against a minimum SDK.

### Host Services Struct

```c
typedef struct {
    uint32_t          struct_size;      // sizeof(SpcHostServices) for forward compat
    SpcAcquireFrameFn acquire_frame;    // NULL if plugin doesn't use frames
    SpcReleaseFrameFn release_frame;    // NULL if plugin doesn't use frames
    SpcLogCallbackFn  log;              // NULL if host doesn't provide logging
    void*             host_ctx;         // single opaque context for all callbacks
    // ... GPU-frame, list-change, and display-size callbacks (see plugin_api.h) ...
    SpcNowMonoNsFn       now_mono_ns;     // shared clock: monotonic ns
    SpcNowUtcNsFn        now_utc_ns;      // shared clock: disciplined UTC ns
    SpcGetTimeFn         get_time;        // shared clock: full reading + sync status
    SpcDisciplineClockFn discipline_clock;// feed a correction (GPS+PPS / NTP)
} SpcHostServices;
```

Read the clock through the `spc_clock.h` helper (see *Timestamps and the shared clock*) rather than calling these pointers directly. The struct is `struct_size`-versioned, so these fields are absent on a pre-0.14 host and the helper falls back to the system clock.

The C++ wrapper `spc::HostServices` provides a convenient interface:

```cpp
struct HostServices {
    const SpcHostServices* svc = nullptr;
    SpcLogContext cached_log{};   // populated by SPC_PLUGIN_HOST_SERVICES macro

    SpcFrame* acquire_frame(uint32_t port, uint32_t w, uint32_t h, SpcPixelFormat fmt);
    void release_frame(SpcFrame* frame);
};
```

### Control Message Helpers

For plugins that send parameter control commands to downstream nodes (`control_msg_helpers.h`, pulled in by `plugin_helpers.h`):

```cpp
SpcControlMsg cmd{};
spc::control_msg_clear(&cmd);
spc::param_cmd_set_int(&cmd, "threshold", 50);
spc::param_cmd_set_float(&cmd, "gain", 1.5f);
spc::param_cmd_set_float64(&cmd, "precise_gain", 1.5);
spc::param_cmd_set_bool(&cmd, "enabled", true);
spc::param_cmd_set_enum(&cmd, "mode", 2);
spc::param_cmd_set_color(&cmd, "tint", 0xFF0000FF);
spc::param_cmd_set_trigger(&cmd, "reset");
spc::param_cmd_set_decimal(&cmd, "price", spc::spc_decimal_make(19999, 2));
```

Each setter returns `false` if the message is full (`SPC_CONTROL_MSG_MAX` entries).

---

## GPU Acceleration (Vulkan Compute)

Plugins can optionally use Vulkan compute shaders for GPU acceleration. The bundled `speculor_gpu` library (headers under `include/speculor_common/gpu/`, included as `<gpu/...>`) provides a `VulkanContext` singleton and utility functions for buffer management, pipeline creation, and command submission.

### Enabling GPU for a Plugin

In your `CMakeLists.txt`:

```cmake
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)

if(SPC_VULKAN_FOUND AND SPC_GLSLANG_VALIDATOR)
    spc_enable_gpu(my_plugin)                              # links speculor_gpu + Vulkan, defines SPC_HAS_VULKAN
    spc_add_gpu_shaders(my_plugin SHADERS my_compute.comp) # compiles GLSL→SPIR-V, embeds as C header
    target_sources(my_plugin PRIVATE my_gpu_pipeline.h my_gpu_pipeline.cpp)
endif()
```

### Key Files

| Header / module | Purpose |
|------|---------|
| `<gpu/vulkan_context.h>` | Shared `VulkanContext` — Vulkan instance, device, compute queue |
| `<gpu/gpu_pipeline_base.h>` | Base class with dispatch, staging, barrier, timing helpers |
| `<gpu/gpu_output_handle.h>` | RAII wrapper for buffer registry handle lifecycle (legacy / `gpu_own_submit()` paths) |
| `<gpu/gpu_ring_output.h>` | `RingOutput` — K-deep edge ring for frames in flight |
| `<gpu/gpu_utils.h>` | Buffer creation, staging, pipeline creation, command helpers |
| `<gpu/gpu_buffer_registry.h>` | Singleton registry mapping opaque handles to Vulkan buffers |
| `<gpu/gpu_failure_tracker.h>` | Standard device-lost / transient-failure policy |
| `PluginHelpers.cmake` | `spc_enable_gpu()` and `spc_add_gpu_shaders()` macros |
| `embed_spirv.cmake` | Converts SPIR-V binaries to C uint32_t array headers |

### GPU Plugin Conventions

Whatever the algorithm — background subtraction, dense optical flow, geometric warps, color conversion — GPU plugins built on this API follow the same conventions:

- Publish GPU-resident output through the engine's K-deep edge ring (`acquire_ringed_output`); the engine owns the staging copy and records it only when an edge has a CPU consumer. `GpuOutputHandle` is retained only for legacy / `gpu_own_submit()` paths.
- Use `cmd_dispatch_compute<T>()` for boilerplate-free shader dispatch
- Record into the engine's secondary command buffer via `record_gpu` + `ScopedExternalRecording`, and do eager init in `start()`
- Check `submit_and_wait()` / `submit_and_spin()` returns and disable GPU on persistent failure
- Expose the engine-managed `gpu_enabled` parameter via `.gpu_compute()` (stop-only toggle, default: true)
- Fall back to CPU when Vulkan is unavailable or the GPU fails
