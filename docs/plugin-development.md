# Plugin Development Guide

A Speculor plugin is a shared library (`.dll` / `.so`) that exports one C object, `spc_plugin`. The host finds plugins in its plugin directory at run time, so a plugin is built on its own and dropped in.

This guide describes SDK 0.59.0, whose plugin ABI number is `SPC_ABI` 59. You need:

- A C++20 compiler (MSVC 19.30+, GCC 12+, Clang 15+) for the SDK's C++ conveniences, or a C11 compiler for a plugin written against the C header alone.
- CMake 3.24+.
- An extracted **Speculor SDK bundle** from the [Releases](https://github.com/speculor-app/speculor-sdk-dist/releases/latest) page. It holds the headers, the prebuilt libraries, every dependency and a CMake package config. Nothing else is needed.

Header paths in this guide are relative to the bundle's include directories: `speculor/...` for the SDK headers (`include/speculor/`), and unprefixed paths such as `gpu/gpu_host.h`, `cv_helpers.h` or `testing/conformance.h` for the shared plugin headers (`include/speculor_common/`), which `spc_add_plugin()` puts on your include path.

The [speculor-plugin-examples](https://github.com/speculor-app/speculor-plugin-examples) repository holds 13 example plugins and a standalone project template. Every sample here follows them.

---

## Your First Plugin

A plugin that outputs a constant scalar, set by its one parameter:

**`scalar_source_plugin.cpp`**

```cpp
#include <speculor/plugin.h>          // the boundary: every type and call that crosses it
#include <speculor/plugin_helpers.h>  // conveniences compiled into the plugin

#include <new>

// The parameters in declaration order. start() and params_changed() get the
// values as one array in this order, so a value's position is its identity.
enum Param : uint32_t {
    P_VALUE,
};

// SPC_PLUGIN_DESCRIPTOR defines info(), which builds the node info on its
// first call and keeps it while the library is loaded.
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("scalar_source", "Scalar Source", "Sources/General")
        .run_on_change()
        .author("You").version("0.1.0")
        .help("Outputs a constant scalar value")
        .float_param("value", "Value", -1e6f, 1e6f, 0.0f, 0.1f)   // P_VALUE
            .param_help("Constant scalar value to output")
        .output("scalar_out", "Scalar", SPC_DATA_SCALAR)
)

struct ScalarSourceState {
    float value = 0.0f;
};

static ScalarSourceState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<ScalarSourceState*>(instance);
}

static SpcPluginInstance* start(SpcHost* /*host*/, const SpcValue* values)
{
    auto* s = new (std::nothrow) ScalarSourceState{};
    if (!s) return nullptr;
    s->value = values[P_VALUE].f;   // a FLOAT value is in .f
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    delete state(instance);
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->value = values[P_VALUE].f;
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    SpcData& out = step->outputs[0];
    out.type = SPC_DATA_SCALAR;
    out.scalar = {};   // timestamp_ns and sequence 0: the engine stamps them
    out.scalar.type = SPC_FIELD_FLOAT;
    out.scalar.f32 = state(instance)->value;
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

**`CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(scalar_source LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SpeculorSDK REQUIRED)
include(PluginHelpers)      # spc_add_plugin(), spc_add_plugin_test(), spc_enable_gpu()
include(CompilerWarnings)   # spc_set_warnings(), which spc_add_plugin() calls

spc_add_plugin(scalar_source SOURCES scalar_source_plugin.cpp)
```

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/SpeculorSDK-<version>-<os>-<arch>
cmake --build build
```

Copy the built `scalar_source.dll` / `scalar_source.so` into the application's plugin directory.

What each part does:

- **`SPC_PLUGIN_DESCRIPTOR(...)`** defines `info()`, the plugin's `info` slot. `DescriptorBuilder` fills the node info: what the plugin is, its ports and its parameters. Pass the builder expression as it is; the macro builds it.
- **`start(host, values)`** makes an instance when the node starts. It gets the node's host and every parameter value, and returns the instance, or null when the node cannot run.
- **`stop(instance)`** tears the instance down and frees it.
- **`params_changed(instance, values, changed)`** tells a running instance the values again when one changes.
- **`process(instance, step)`** runs one step: it reads `step->inputs` and fills `step->outputs`.
- **`SPC_EXPORT_PLUGIN(...)`** defines `spc_plugin` from the slots listed. A slot left out is null: a call the plugin does without.

---

## The Boundary

### One header, one object, one number

- **One header.** `speculor/plugin.h` declares everything that crosses between the engine and a plugin, in C11, with no other include than `stdint.h`. Everything else in the SDK (the builder, the macros, the table and GPU helpers) is compiled into your plugin and never crosses.
- **One object.** A plugin library exports `SpcPlugin spc_plugin`: a table of slots, the functions the engine calls.
- **One number.** The first field of `spc_plugin` is `abi`. The engine reads it before anything else and loads the plugin only when it equals its own `SPC_ABI`. Any change to the header that a plugin could notice changes the number, so build against the SDK bundle that matches the application release you target. `SPC_SDK_VERSION` (`speculor/version.h`) versions the C++ conveniences and the bundle; it does not decide whether a plugin loads.
- **Strings and arrays by pointer.** Every string is nul-terminated `const char*`; those of the node info are never null (`""` when there is nothing to say). Every array comes with its count. Whoever makes a string or an array owns it, and each stays within the limits the header names.
- **Stable numbers.** `SpcDataType` values are stored in projects and sessions, and the order of `SpcLicenseTier` is the licence gate. Neither is ever renumbered.

### The slots

`SpcPlugin` declares its slots in this order:

| Slot | Required | What it does |
|---|---|---|
| `info` | yes | Returns the node info. Called once, when the library is discovered |
| `start` | yes | Makes an instance from the host and every parameter value |
| `stop` | yes | Tears the instance down and frees it |
| `process` | | Runs one step |
| `interrupt` | | Phase 1 of shutdown: cut blocking I/O ([Two-Phase Shutdown](#two-phase-shutdown)) |
| `params_changed` | | Tells a running instance the values again |
| `record_gpu` | for `SPC_GPU_RECORDED` and `SPC_GPU_OWN_SUBMIT` | Records one step's GPU work ([GPU Plugins](#gpu-plugins)) |
| `ui_input` | | A mouse or key input on the node's view |
| `control_reply` | | A reply to a control command the instance sent |
| `param_rules` | | Which parameters the values leave disabled or missing |
| `list_options` | | The options of an ENUM parameter that only looking can find |

### Exporting

`SPC_EXPORT_PLUGIN(...)` (`plugin.h`) defines `spc_plugin` with `abi` set to `SPC_ABI`, from the slots it lists. It works in C and in C++.

- List only the slots you implement, in the order of the table above. C++20 designated initializers must follow declaration order.
- The macro ends the definition itself: write no semicolon after it.
- An `#ifdef` cannot go inside a macro's arguments. For a slot that only a Vulkan build has, write `SPC_IF_VULKAN(.record_gpu = record_gpu,)` (`plugin_macros.h`): it expands to its argument when `SPC_HAS_VULKAN` is defined, and to nothing otherwise.

C++ plugins can also use the SDK's composition layer, which builds `start` and `stop` from functions of another shape. It exports with `SPC_EXPORT_PLUGIN_SLOTS(...)`; see [The C++ Composition Layer](#the-c-composition-layer).

### A plugin in C

`plugin.h` alone is enough to write a plugin in C, or to bind one from another language. Without the C++ builder, the node info is filled by hand:

```c
#include <speculor/plugin.h>

#include <stdlib.h>

enum { P_VALUE };

static const SpcParam kParams[] = {
    {
        .id = "value", .label = "Value", .group = "", .help = "The value to output",
        .type = SPC_PARAM_FLOAT,
        .def = {.f = 0.0f}, .min = {.f = -1e6f}, .max = {.f = 1e6f}, .step = {.f = 0.1f},
    },
};

static const SpcPort kOutputs[] = {
    {.id = "value_out", .label = "Value", .type = SPC_DATA_SCALAR},
};

static const SpcNodeInfo kInfo = {
    .id = "c_constant", .label = "C Constant", .category = "Sources/General",
    .author = "You", .version = "0.1.0", .help = "Outputs a constant scalar",
    .traits = SPC_NODE_RUN_ON_CHANGE,
    .outputs = kOutputs, .output_count = 1,
    .params = kParams, .param_count = 1,
};

typedef struct { float value; } Constant;

static const SpcNodeInfo* info(void) { return &kInfo; }

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    (void)host;
    Constant* c = malloc(sizeof *c);
    if (!c) return NULL;
    c->value = values[P_VALUE].f;
    return (SpcPluginInstance*)c;
}

static void stop(SpcPluginInstance* instance) { free(instance); }

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    SpcData* out = &step->outputs[0];
    out->type = SPC_DATA_SCALAR;
    out->scalar = (SpcScalar){.type = SPC_FIELD_FLOAT, .f32 = ((Constant*)instance)->value};
    return SPC_OK;
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values, uint64_t changed)
{
    (void)changed;
    ((Constant*)instance)->value = values[P_VALUE].f;
}

SPC_EXPORT_PLUGIN(.info = info, .start = start, .stop = stop, .process = process,
                  .params_changed = params_changed)
```

Four structs in the header hold an anonymous union (`SpcValue`, `SpcScalar`, `SpcData`, `SpcUiInput`), so C11 is the minimum. Build the library with `SPC_PLUGIN_EXPORT` defined and every symbol but `spc_plugin` hidden; `spc_add_plugin()` does this for C++ sources.

---

## Lifecycle

```
library loaded, abi checked
  -> info()                          once, at discovery
  -> start(host, values)             returns an instance
  -> running: process / record_gpu,
              params_changed, ui_input, control_reply
  -> interrupt()                     optional, from another thread
  -> stop(instance)                  frees it
  -> start(...) again on the next run
```

- **An instance exists only while its node runs.** `start()` makes it and `stop()` frees it. Stop and Play, and a restart after a crash or a hang, each make a new one. Whatever a plugin keeps between runs, it keeps in its parameters.
- **`start(host, values)`** gets the host it will use and every parameter value in declaration order (`values` is null when the plugin declares none). It returns the instance, or null after saying why through `host->log`. When it returns null, nothing else is called, `stop()` included, so it undoes what it made.
- **`stop(instance)`** comes after the node's thread has left the plugin. Nothing is called on the instance afterwards.
- **Several instances of one plugin may run at once**, one per node, and an abandoned instance may still be running when its replacement starts (see [Health and the Watchdog](#health-and-the-watchdog)). Keep state in the instance, not in globals.

---

## Threads

### Calls into the plugin

| Call | Thread | Alongside |
|---|---|---|
| `info` | the loader, once, at discovery | nothing |
| `start` | an engine thread, before the node's steps; given up on after 30 s | nothing of this instance |
| `process`, `record_gpu` | the node's thread | `interrupt` only |
| `params_changed` | the node's thread, between steps | `interrupt` only |
| `ui_input` | the node's thread, between steps (the host queues it) | `interrupt` only |
| `control_reply` | the node's thread, between steps | `interrupt` only |
| `interrupt` | any engine thread, before the engine waits for the node's thread | a step in progress |
| `stop` | an engine thread, after the node's thread left the plugin | nothing |
| `param_rules` | any thread, any time; no instance | anything |
| `list_options` | off the host's GUI thread; no instance | anything |

The engine calls an instance one call at a time, `interrupt` aside, so the plugin needs no lock for its own state on the engine's account. `start()` and `stop()` run on an engine thread, not the node's: anything a plugin sets up per thread (a thread-local log sink, a COM apartment) belongs at the top of `process()`.

### Host functions

`SpcHost` is the table of functions the engine lends each running node. The plugin gets it in `start()` and may keep it until `stop()` returns. Every function takes the host itself as its first argument, and **every function is set**: one that does not apply to the node answers no (`gpu()` returns null while the node's GPU is off, `gpu_frame()` on a node with no GPU frames to lend), so a plugin calls it without checking it first.

Each function may be called only from where the header says. "Any plugin thread" is any thread the plugin is called on or owns, until `stop()` returns.

| Function | What it does | Where |
|---|---|---|
| `log(host, level, message)` | A log line, attributed to the node | any plugin thread |
| `now_mono_ns(host)` | Monotonic nanoseconds; never steps back | any plugin thread |
| `now_utc_ns(host)` | Disciplined UTC nanoseconds, the timeline of every timestamp | any plugin thread |
| `time(host, &info)` | Both timelines, the sync source, its error bound and lock state | any plugin thread |
| `discipline_clock(host, &correction)` | Steers the engine clock from a reference the plugin reads | any plugin thread |
| `frame(host, out, w, h, format)` | A frame from the engine's pool for an output; null when none can be had | `process`, `record_gpu` |
| `release_frame(host, frame)` | Gives back a frame from `frame()` or `gpu_frame()` that is not put on an output | `process`, `record_gpu` |
| `table(host, out, rows)` | A zeroed table of an output's schema, lent for the step | `process`, `record_gpu` |
| `set_output_schema(host, out, schema)` | Columns decided at run time for a table or signal output | `start`, `process` |
| `control(host, out, kind, param, type, value, request_id)` | A command on a control output | `process`, `record_gpu` |
| `take_signal(host, in)` | The next block waiting on a signal input, or null | one thread at a time per port: the node's, or one the plugin owns |
| `emit_event(host, &event)` | An event for the session and the GUI | `process`, `record_gpu`, `ui_input` |
| `emit_event_snapshot(host, &event, frame, x, y, w, h)` | The same, with a thumbnail cropped from a frame | `process`, `record_gpu`, `ui_input` |
| `set_param(host, param, &value)` | Sets one of the plugin's own parameters | any plugin thread |
| `set_param_view(host, param, &view)` | How a parameter presents while the instance lives | any plugin thread |
| `display_size(host, &w, &h)` | The pixel size of the widget showing the node; non-zero when no widget shows it | any plugin thread |
| `gpu(host)` | The device the host lends; null when the node's GPU is off | any plugin thread |
| `gpu_frame(host, out, w, h, format)` | A GPU-resident output frame; null when the node's GPU is off or it has no Vulkan device | `process`, `record_gpu` |
| `ensure_cpu_data(host, frame)` | Gives a GPU-resident frame its CPU pixels; non-zero when it cannot | `process`, `record_gpu` |
| `gpu_buffer(host, handle, &buffer)` | The buffer behind a frame's `gpu_handle` | `process`, `record_gpu` |
| `gpu_share_buffer(host, &buffer, cpu_copy)` | Offers a buffer of the plugin's own downstream; returns its handle | `process`, `record_gpu` |
| `gpu_unshare_buffer(host, handle)` | Withdraws a shared buffer | `process`, `record_gpu`, `stop` |
| `gpu_read_buffer(host, handle, dest, size)` | Copies the bytes behind a handle to CPU memory; waits for the GPU | `process`, `record_gpu` |
| `gpu_time(host, microseconds)` | GPU time the engine cannot see, for the node's statistics | any plugin thread |

`node_label` is a field, not a function: the node's name, valid until `stop()` returns.

The conformance harness checks every one of these rules ([Testing Your Plugin](#testing-your-plugin)).

---

## The Node Info

`SpcNodeInfo` says what the plugin is: its identity, its ports and its parameters. It belongs to the plugin and lives, unchanged, while the library is loaded. In C++, `SPC_PLUGIN_DESCRIPTOR(builder)` defines `info()` from a `spc::DescriptorBuilder`:

```cpp
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_filter", "My Filter", "Filters/Image")
        .author("You").version("1.0.0")
        .help("Does something useful")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"image"})
        .input("in", "Input", SPC_DATA_FRAME)
        .output("out", "Output", SPC_DATA_FRAME)
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)
            .param_help("Multiplier applied to every pixel")
)
```

- **`id`** identifies the plugin in saved projects. Never change it.
- **`.maturity(m)`**: `SPC_MATURITY_EXPERIMENTAL` (the default), `_PREVIEW` or `_STABLE`. The plugin browser filters on it.
- **`.license_tier(t)`**: the lowest licence tier that may use the plugin, `SPC_LICENSE_COMMUNITY` (the default, everyone) up to `SPC_LICENSE_ENTERPRISE`. The host does not offer a plugin whose tier is above the active licence.
- **`.tags({...})`**: domain tags, at most `SPC_TAG_MAX`.

### Traits

| Trait | Builder | Meaning |
|---|---|---|
| `SPC_NODE_RUN_ON_CHANGE` | `.run_on_change()` | Runs once, then again only when a parameter changes |
| `SPC_NODE_SOURCE` | `.source()` | Its data comes from outside the pipeline: a device, a file, a network stream, a generator. A recording captures it, and replay feeds it from the session instead of running the node. A source without it is neither recorded nor replayed |
| `SPC_NODE_LIVE_ONLY` | `.live_only()` | `process()` has an effect outside the pipeline that cannot be undone: network egress, a recorder, a service feed. Replay skips the node, so a session is never sent or recorded twice. Terminal sinks only |

### Ports

Inputs and outputs are numbered separately, each from zero in declaration order, at most `SPC_PORT_MAX` of each.

| Builder | Port |
|---|---|
| `.input(id, label, type)` / `.output(id, label, type)` | A port of type `SPC_DATA_FRAME`, `SPC_DATA_SCALAR`, `SPC_DATA_JSON` or `SPC_DATA_PACKET` |
| `.input(id, label, type, capacity, mode)` | An input with a queue capacity and a consume mode |
| `.input_table(id, label, {fields} [, capacity, mode])` / `.output_table(id, label, {fields})` | A table port and its fields |
| `.input_signal(id, label, {fields})` / `.output_signal(id, label, {fields})` | A signal port and its fields |
| `.optional_fields({fields})` | More fields for the input just added, each read only when the producer sends it |
| `.output_control(id, label)` | A control output (`SPC_DATA_CONTROL`) |
| `.gpu_ok()` | The input just added takes GPU-resident frames as they are (`SPC_INPUT_GPU_OK`): the engine does not download them first |

Fields are `{id, type}` pairs: `{"x", SPC_FIELD_FLOAT}`. A table field is a fixed-size type: `SPC_FIELD_FLOAT`, `_FLOAT64`, `_INT8` to `_INT64`, `_UINT8` to `_UINT64`, `_BOOL`, `_DECIMAL`, or `_STRING32` / `_64` / `_128` / `_256`.

An input declares every field it reads. A connection whose producer lacks a required field is refused; an optional field the producer lacks reads as missing.

Consume modes:

- `SPC_CONSUME_FIFO`: every datum, in order. The node waits for it.
- `SPC_CONSUME_LATEST`: the newest datum, dropping older ones. The node waits for it. The queue holds one datum whatever the capacity.
- `SPC_CONSUME_NON_BLOCKING`: whatever has arrived, without waiting; the input is `SPC_DATA_NONE` when nothing has. Use it for side inputs that update state without driving the node (a GPS position, a mask). A node whose inputs are all non-blocking runs at its own pace, like a source.
- `SPC_CONSUME_DEFAULT`: the engine decides.

### Parameters

| Builder | Type | The value, in `SpcValue` |
|---|---|---|
| `.int_param(id, label, min, max, default, step [, group])` | `SPC_PARAM_INT` | `.i` |
| `.float_param(...)` | `SPC_PARAM_FLOAT` | `.f` |
| `.float64_param(...)` | `SPC_PARAM_FLOAT64` | `.d` |
| `.bool_param(id, label, default [, group])` | `SPC_PARAM_BOOL` | `.i`, 0 or 1 |
| `.string_param(id, label, default [, group])` | `SPC_PARAM_STRING`, up to `SPC_PARAM_STRING_MAX` | `.str.ptr`, `.str.len` |
| `.text_param(id, label, default [, group])` | `SPC_PARAM_TEXT`, multi-line, up to `SPC_PARAM_TEXT_MAX` | `.str.ptr`, `.str.len` |
| `.enum_param(id, label, {labels}, default_index [, group])` | `SPC_PARAM_ENUM` | `.choice.index`, `.choice.id` |
| `.color_param(id, label, rgba [, group])` | `SPC_PARAM_COLOR` | `.color`, `0xRRGGBBAA` |
| `.decimal_param(id, label, min, max, default, step [, group])` | `SPC_PARAM_DECIMAL` | `.decimal` |
| `.trigger_param(id, label [, group])` | `SPC_PARAM_TRIGGER`, a push button | `.i`, 1 in the call it fired in |
| `.list_param(id, label [, group])`, then `.list_column(...)` / `.list_column_enum(...)` | `SPC_PARAM_LIST`, rows the user edits | `.list.cells`, `.list.rows`, `.list.cols` |

After a parameter:

- `.param_help(text)` sets its tooltip.
- `.mandatory()`: the pipeline does not start until it holds a non-empty value.
- `.listed()` on an ENUM: its options come from `list_options` ([Listing options](#listing-options)).
- `.flag(bits)` adds other flags: `SPC_PARAM_FLAG_STOP_ONLY` (it cannot change while the pipeline runs), `SPC_PARAM_FLAG_STREAM_BINDING` (on a source's STRING: its value names the stream the node carries, which recordings and replay match by).

Projects save values by parameter id, so never change a shipped parameter's id, default or range. A plugin that uses a GPU does not declare `gpu_enabled` or `gpu_profile`: the engine adds and owns them ([GPU Plugins](#gpu-plugins)).

---

## Parameters at Run Time

The engine holds every value. It checks a new value against the declaration, saves it with the project and shows it. The plugin is told the values and is never asked for them.

```cpp
enum Param : uint32_t { P_MODE, P_THRESHOLD, P_LABEL };   // declaration order

struct Params {
    int32_t mode;
    float threshold;
    std::string label;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.mode      = values[P_MODE].choice.index;   // ENUM: the position of the option
    p.threshold = values[P_THRESHOLD].f;
    p.label.assign(values[P_LABEL].str.ptr, values[P_LABEL].str.len);   // copy: valid during the call only
    return p;
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t changed)
{
    auto* s = state(instance);
    s->params = read_params(values);
    if (changed & (uint64_t{1} << P_MODE)) s->rebuild = true;
}
```

- **`start()` and `params_changed()` get the same array**: one value per declared parameter, in declaration order. Bit `i` of `changed` is set for each value that differs from what the instance last had. A TRIGGER's bit is set in the one call it fired in.
- **`params_changed` runs on the node's thread, between steps**, so the plugin reads its values without a lock. A plugin that only reads its parameters in `start()` leaves the slot out; a change then takes effect on the next run.
- **Values live during the call.** Copy a STRING, TEXT or LIST you keep.
- **An ENUM restored by id** that is not among the options has the position one past the last option.
- **A LIST** holds `rows * cols` values, row after row, each cell in the member its column's type names.

### Changing a value from the plugin

A value the plugin settled on itself (an auto-exposure result, a selection made on its own panel, a device's real rate), it sets with `set_param`. The host stores, saves and shows it, and does not report it back through `params_changed`. How a parameter presents while the instance lives, it sets with `set_param_view`:

```cpp
SpcValue rate{};
rate.i = device_rate;
host->set_param(host, P_SAMPLE_RATE, &rate);

SpcParamView view{};
view.fields = SPC_PARAM_VIEW_RANGE;   // _STATE, _RANGE and _OPTIONS name what is set
view.min = 1.0;
view.max = device_max_gain;
view.step = 1.0;
host->set_param_view(host, P_GAIN, &view);
```

### Parameter rules

`param_rules(values, states)` says which parameters the values leave disabled or missing. It is a function of the values alone: no instance, any thread. `states` has one entry per parameter; each arrives 0, or `SPC_PARAM_STATE_MISSING` for an empty mandatory STRING or TEXT, and the plugin sets or clears bits:

```cpp
static void param_rules(const SpcValue* values, uint32_t* states)
{
    if (values[P_ENABLE_WEIGHT].i == 0) {
        states[P_WEIGHT1] |= SPC_PARAM_STATE_DISABLED;
        states[P_WEIGHT2] |= SPC_PARAM_STATE_DISABLED;
    }
    if (values[P_SOURCE].choice.index == 1 && values[P_URL].str.len == 0)
        states[P_URL] |= SPC_PARAM_STATE_MISSING;   // the pipeline cannot start
}
```

### Listing options

An ENUM whose options are found only by looking (devices, serial ports, files) is flagged `.listed()`, and the plugin implements `list_options`. It has no instance and runs off the GUI thread:

```cpp
static int list_options(uint32_t param, const SpcValue* /*values*/, int refresh,
                        SpcHost* host, SpcOptionSinkFn sink, void* sink_ctx)
{
    if (param != P_DEVICE) return SPC_NOT_FOUND;
    if (refresh || !g_scanned) scan_devices(host);   // may take seconds
    for (const Device& d : g_devices)
        sink(sink_ctx, d.serial, d.name);            // id saved with the project, label shown
    return SPC_OK;
}
```

- With `refresh` 0, answer from the last scan, and scan only if there has never been one. With `refresh` set, the user asked: look again.
- `host` may be null. When it is not, no node runs, so only its `log` is for this call.
- `SPC_OK` with no call to `sink` means there is no option now. Any other result means the plugin could not look, and the options the host holds stand.
- The declared options are a placeholder until the plugin has listed. A saved choice is restored by its id.

---

## A Step

`process(instance, step)` runs one step. `SpcStep` holds one `SpcData` per input port and one per output port, in port order:

```c
typedef struct {
    const SpcData* inputs;
    uint32_t       input_count;
    SpcData*       outputs;
    uint32_t       output_count;
} SpcStep;
```

- **Inputs** are the host's, borrowed for the call. An input with nothing this step is `SPC_DATA_NONE`; check `type` before use. A signal input's slot is always `SPC_DATA_NONE`: its blocks are taken from the host ([Signals](#signals)).
- **Outputs** arrive as `SPC_DATA_NONE`. The plugin sets the type and the datum of each output it produces; an output left `SPC_DATA_NONE` emits nothing. `consumers` on an output is `SPC_PORT_CONSUMERS_NONE` when nothing reads the port this step, so a plugin can skip costly work such as a debug view.
- **Every datum carries a timestamp and a sequence.** `timestamp_ns` is disciplined UTC on the engine's one clock ([The Clock](#the-clock)). `sequence` is the sequence of what the datum was made from, so data of different kinds can be matched. A filter copies both from its input. A datum left with 0 in either is stamped by the engine as it publishes it: the clock's time, and the sequence of the input the node is processing.

| Result | Value | Meaning |
|---|---|---|
| `SPC_OK` | 0 | Done |
| `SPC_SKIP` | -6 | Nothing this time, not an error. No backoff; the engine calls again after a short poll |
| `SPC_FAILED` | -1 | Failed. The engine backs off, from 50 ms up to 5 s; a parameter change retries at once |
| `SPC_DEVICE_LOST` | -5 | The GPU is gone. No backoff: the watchdog takes over |
| `SPC_INVALID` | -4 | Invalid argument or state |
| `SPC_NOT_FOUND` | -3 | No such parameter or resource |
| `SPC_TRUNCATED` | -7 | A value was written cut to fit its field |

From `process()`, every non-zero result other than `SPC_SKIP` and `SPC_DEVICE_LOST` takes the error path, with backoff. `record_gpu` gets the same step, plus the engine's `SpcGpuPass` ([GPU Plugins](#gpu-plugins)).

---

## Data Types

| Type | Enum | An output | An input |
|---|---|---|---|
| Frame | `SPC_DATA_FRAME` | from `host->frame()`, published without a copy; a frame of the plugin's own is copied | `SpcFrame`, CPU pixels and/or a GPU handle |
| Table | `SPC_DATA_TABLE` | only from `host->table()`, published without a copy | with `field_offsets` for the fields the input declares |
| Signal | `SPC_DATA_SIGNAL` | a table from `host->table()`; the host copies each block for every reader | taken with `host->take_signal()` |
| JSON | `SPC_DATA_JSON` | the plugin's own string, copied | `SpcJson` |
| Packet | `SPC_DATA_PACKET` | the plugin's own payload, copied | `SpcPacket` |
| Scalar | `SPC_DATA_SCALAR` | by value | by value |
| Control | `SPC_DATA_CONTROL` | commands through `host->control()`; replies come to `control_reply` | the host applies them: the plugin sees parameter changes |

The engine refuses an output over its limit rather than copy it: a table over `SPC_TABLE_MAX_BYTES` (256 MiB), a JSON string over `SPC_JSON_MAX_BYTES` (16 MiB), a packet over `SPC_PACKET_MAX_BYTES` (64 MiB). A refused output counts as a delivery failure and is logged once per port.

### Frames

```c
typedef struct {
    uint8_t*        data;          // pixels; null when the frame is GPU-only
    uint32_t        width, height;
    uint32_t        stride;        // bytes per row, padding included
    SpcPixelFormat  format;        // GRAY8, RGB24, BGR24, RGBA32, NV12, FLOAT32, GRAY16, RGB48
    SpcBayerPattern bayer_pattern;
    int64_t         timestamp_ns;
    uint64_t        sequence;
    uint32_t        pool_id;       // non-zero on a frame from host->frame()
    uint64_t        gpu_handle;    // 0 = CPU only
    uint32_t        gpu_flags;     // SPC_GPU_FLAG_RESIDENT, SPC_GPU_FLAG_CPU_VALID
} SpcFrame;
```

A filter writes its output into a frame from the engine's pool, which is published without a copy. When no pool frame can be had, a frame of the plugin's own still works, at the cost of a copy:

```cpp
static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const SpcData& in_data = step->inputs[0];
    if (in_data.type != SPC_DATA_FRAME || !in_data.frame) return SPC_INVALID;
    const SpcFrame* in = in_data.frame;
    if (in->format != SPC_PIXEL_FORMAT_RGB24) return SPC_INVALID;
    const uint32_t w = in->width, h = in->height;

    SpcFrame* out = host->frame(host, 0, w, h, SPC_PIXEL_FORMAT_RGB24);
    if (!out) s->buffer.resize(static_cast<size_t>(w) * h * 3);
    uint8_t* dst = out ? out->data : s->buffer.data();
    const uint32_t dst_stride = out ? out->stride : w * 3;

    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src_row = in->data + static_cast<size_t>(y) * in->stride;
        uint8_t* dst_row = dst + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < w * 3; ++x)
            dst_row[x] = static_cast<uint8_t>(std::min(255.0f, src_row[x] * s->gain));
    }

    SpcData& out_data = step->outputs[0];
    out_data.type = SPC_DATA_FRAME;
    if (out) {
        out->timestamp_ns = in->timestamp_ns;
        out->sequence = in->sequence;
        out_data.frame = out;   // the engine's from here on, whatever process() returns
    } else {
        s->own = SpcFrame{};
        s->own.data = s->buffer.data();
        s->own.width = w;
        s->own.height = h;
        s->own.stride = w * 3;
        s->own.format = SPC_PIXEL_FORMAT_RGB24;
        s->own.timestamp_ns = in->timestamp_ns;
        s->own.sequence = in->sequence;
        out_data.frame = &s->own;   // copied by the engine
    }
    return SPC_OK;
}
```

- **A pool frame on an output belongs to the engine** from then on, whatever `process()` returns. Do not keep using it.
- **A pool frame not put on an output** goes back with `host->release_frame(host, frame)`.
- **An input frame passed on unchanged** is shared, not copied: put the input's `SpcFrame*` on the output.
- **A GPU-resident input** has `gpu_handle` set and `SPC_GPU_FLAG_RESIDENT`. Unless the input is `.gpu_ok()`, the engine gives a CPU plugin the pixels first. A `.gpu_ok()` input that the plugin reads on the CPU after all calls `host->ensure_cpu_data(host, frame)` first.

### Tables

A table is a flat array of records described by a schema, for data on the hot path: detections, tracks, readings. An output table comes from the host: `host->table(host, out, rows)` lends `rows` zeroed records of the output's schema, the host's until `process()` returns. Putting it in the step's outputs publishes it without a copy; a lent table not emitted goes back to the host. The plugin may lower `record_count`, and must not keep, resize or free the table. A plugin emits no other table.

```cpp
enum Output : uint32_t { OUT_BOXES };
enum { F_X, F_Y, F_W, F_H, F_SCORE };   // the output's fields, in declaration order

// .output_table("boxes", "Boxes", {{"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT},
//                                  {"w", SPC_FIELD_FLOAT}, {"h", SPC_FIELD_FLOAT},
//                                  {"score", SPC_FIELD_FLOAT}})

SpcTable* out = host->table(host, OUT_BOXES, count);
if (!out) return SPC_FAILED;
const uint32_t* off = out->field_offsets;   // where each field is in a record
for (uint32_t i = 0; i < count; ++i) {
    spc_table_set_float(out, i, off[F_X], boxes[i].x);
    spc_table_set_float(out, i, off[F_Y], boxes[i].y);
    spc_table_set_float(out, i, off[F_W], boxes[i].w);
    spc_table_set_float(out, i, off[F_H], boxes[i].h);
    spc_table_set_float(out, i, off[F_SCORE], boxes[i].score);
}
out->timestamp_ns = in->timestamp_ns;
out->sequence = in->sequence;
step->outputs[OUT_BOXES].type = SPC_DATA_TABLE;
step->outputs[OUT_BOXES].table = out;
```

`host->table()` returns null for a port that is not a table or signal output, or a size over `SPC_TABLE_MAX_BYTES`.

An input table is laid out by its producer. Its `field_offsets` has one entry per field the **input** declares, in declaration order, so the plugin indexes it with an enum of its own fields. An optional field the producer does not send is `SPC_NO_FIELD`:

```cpp
// .input_table("boxes_in", "Boxes", {{"x", SPC_FIELD_FLOAT}, {"y", SPC_FIELD_FLOAT}})
//     .optional_fields({{"class_name", SPC_FIELD_STRING32}})
enum { F_X, F_Y, F_CLASS_NAME };

const SpcData& in = step->inputs[IN_BOXES];
if (in.type == SPC_DATA_TABLE && in.table) {
    const SpcTable* tbl = in.table;
    const uint32_t* off = tbl->field_offsets;
    for (uint32_t i = 0; i < tbl->record_count; ++i) {
        const float x = spc_table_get_float(tbl, i, off[F_X]);
        const float y = spc_table_get_float(tbl, i, off[F_Y]);
        const char* name = off[F_CLASS_NAME] != SPC_NO_FIELD
                               ? spc_table_get_string(tbl, i, off[F_CLASS_NAME]) : "";
        // ...
    }
}
```

**Columns decided at run time** (a database query's result): declare the output with no fields, then give it a schema with `set_output_schema` from `start()` or `process()`, again whenever the columns change. The host keeps a copy:

```cpp
spc::SchemaStorage columns;   // schema_storage.h owns the fields and their ids
columns.add_field("name", SPC_FIELD_STRING64);
columns.add_field("value", SPC_FIELD_FLOAT64);
host->set_output_schema(host, OUT_ROWS, columns.get());
```

The table accessors are in `speculor/table_helpers.h`: `spc_table_get_*` and `spc_table_set_*` for every field type, `spc_table_get_string` / `spc_table_set_string` for fixed-size strings, and `spc_table_walk` to visit every field of every record.

**Decimal fields** (`SPC_FIELD_DECIMAL`, `SpcDecimal`) hold exact values: a 64-bit mantissa and a scale, value = mantissa / 10^scale. `spc::spc_decimal_make(19999, 2)` is 199.99, and `spc::spc_decimal_to_string()` formats one (`decimal_helpers.h`).

### Signals

A signal carries sample blocks (audio, SDR I/Q) too fast to step the consumer once per block. The producer emits each block as a table lent by the host, with the output's type set to `SPC_DATA_SIGNAL`. The host copies the block for every reader and holds it until the reader takes it, so nothing of the consumer runs on the producer's thread.

Producer:

```cpp
SpcTable* out = host->table(host, OUT_SAMPLES, count);
if (!out) return SPC_FAILED;
const uint32_t* off = out->field_offsets;
for (uint32_t i = 0; i < count; ++i)
    spc_table_set_float(out, i, off[F_AMPLITUDE], next_sample());
out->sample_rate_hz = sample_rate;   // per-block metadata: rate, frequency, gain...
out->timestamp_ns = host->now_utc_ns(host);
out->sequence = ++s->blocks;
step->outputs[OUT_SAMPLES].type = SPC_DATA_SIGNAL;
step->outputs[OUT_SAMPLES].table = out;
```

Consumer, at the top of `process()`:

```cpp
while (const SpcTable* block = host->take_signal(host, IN_SIGNAL)) {
    const uint32_t off = block->field_offsets[F_IN_AMPLITUDE];
    for (uint32_t i = 0; i < block->record_count; ++i)
        push(spc_table_get_float(block, i, off));
}
```

- A block is valid until the next `take_signal` on the same port.
- One thread at a time takes from a port: the node's own, or one the plugin owns, such as an audio device callback.
- The host holds `SPC_SIGNAL_QUEUE_RECORDS` records per signal input. When a slower reader lets that fill, the block that does not fit is dropped whole.
- A mono input (`{amplitude}`) can read a stereo output (`{amplitude, right}`): the input reads the fields it declares.

### JSON

`SpcJson` carries a JSON string, for metadata off the hot path. The string is the plugin's own; the engine copies it after `process()` returns, so keep it in the instance:

```cpp
s->json_text = make_json(...);   // a std::string member
s->json.json = s->json_text.c_str();
s->json.length = static_cast<uint32_t>(s->json_text.size());
step->outputs[OUT_META].type = SPC_DATA_JSON;
step->outputs[OUT_META].json = &s->json;
```

### Packets

`SpcPacket` carries a compressed bitstream captured before decoding, for recording or forwarding video without encoding it again. H.264 and HEVC payloads are Annex-B. `flags` marks keyframes (`SPC_PACKET_FLAG_KEYFRAME`), codec configuration (`_CONFIG`), timeline breaks (`_DISCONTINUITY`), audio (`_AUDIO`, with `sample_rate` and `channels`) and batched audio frames (`_BATCH`). After a gap a consumer discards packets until the next keyframe. The payload is the plugin's own and is copied; packet inputs are always FIFO.

### Scalars

A scalar is one typed value: set `scalar.type` to an `SpcFieldType` and the matching member (`f32`, `f64`, `i32`, `b`, `dec`, ...). Reading an input, `spc::input_scalar(step->inputs, step->input_count, port)` returns it as a float whatever its type (`io_helpers.h`).

### Control

A control output sends commands to the nodes connected to it: set a parameter, ask for one or all of them, apply a preset. The plugin never builds a message; it calls the host:

```cpp
SpcValue v{};
v.f = 1.5f;
host->control(host, OUT_CMD, SPC_CONTROL_SET, "gain", SPC_PARAM_FLOAT, &v, /*request_id*/ 0);

// the same through the SDK's convenience
spc::ControlOut(host, OUT_CMD, 0).set_float("gain", 1.5f);
```

- `kind` is `SPC_CONTROL_SET`, `_GET`, `_LIST` or `_PRESET`. Add `SPC_CONTROL_AS_USER` to a SET or PRESET that is the operator's own edit, made on the sender's panel: the project keeps it like a change made in Node Properties.
- An ENUM is set by position (`choice.index`).
- The host sends the commands once `process()` returns. Commands of one kind and request id made in one step travel and are answered together.
- With `request_id` 0 a SET gets no reply. With another id the reply comes to the `control_reply` slot, on the node's thread between steps: `SpcControlReply` says, per parameter, whether the target took it (ACK), or holds the values and declarations asked for (VALUES). It is valid during the call only. `spc::control_ack_find(reply, name, &rc)` reads one ACK entry.

The target plugin does nothing to receive commands: the host applies them to its parameters, and it sees `params_changed`.

### Events

An event marks something notable: a track acquired, an object entering a zone, a threshold crossed. The host stamps it on the shared clock, attributes it to the node, records it in the session and shows it in the GUI. Events are for low-frequency moments; per-frame data belongs in the node's outputs.

```cpp
SpcEvent e{};
e.category = SPC_FOURCC('T', 'H', 'R', 'S');   // a kind of the plugin's own
e.severity = SPC_EVENT_WARNING;
e.phase = SPC_EVENT_BEGIN;                     // BEGIN and END with one id make a span
e.id = s->next_id++;
e.label = "Above threshold";
e.detail = detail;                             // the host copies the strings during the call
host->emit_event(host, &e);
```

- `timestamp_ns` and `sequence` left 0 are stamped by the host: the clock's time and the sequence of the input being processed. A source sets the sequence of the frame the event is about.
- `SPC_EVENT_HAS_REGION` with `x, y, w, h` (normalized) marks a region of the frame; `SPC_EVENT_HAS_GEO` with `lat, lon, alt` a place.
- A string longer than its limit (`SPC_EVENT_LABEL_MAX`, `_DETAIL_MAX`, `_DATA_MAX`, nul included) is refused, not cut.
- `emit_event_snapshot` also attaches a thumbnail: the host crops the normalized region from the frame's CPU pixels, scales and encodes it on a worker thread, and returns at once.

### UI input

A plugin whose view the user clicks or types into implements `ui_input`. The host queues each input and calls the slot on the node's thread, between steps:

```cpp
static void ui_input(SpcPluginInstance* instance, const SpcUiInput* input)
{
    auto* s = state(instance);
    if (input->type == SPC_UI_MOUSE_PRESS && input->mouse.button == SPC_MOUSE_BUTTON_LEFT)
        s->click = {input->mouse.x, input->mouse.y};   // normalized [0,1] over the frame
    else if (input->type == SPC_UI_KEY_PRESS && input->key.key_code == SPC_KEY_ESCAPE)
        s->click.reset();
}
```

`display_size` gives the pixel size of the widget showing the node, for a plugin that relates frame pixels to screen pixels.

### Ownership

| What | Owned by | Valid until |
|---|---|---|
| `SpcNodeInfo` and all it points at | the plugin | the library is unloaded, unchanged |
| a port's schema | the plugin | the library is unloaded |
| a schema passed to `set_output_schema` | the caller; the host keeps a copy | the call returns |
| the step's inputs | the host | the call returns |
| a table from `host->table()` | the host | the call returns; emitted, it is published |
| a frame from `host->frame()` | the host | put on an output, it is the engine's; otherwise until released |
| a block from `take_signal` | the host | the next `take_signal` on the port |
| an event and its strings | the caller; the host copies them | the call returns |
| `SpcHost` | the host | `stop()` returns |
| `SpcGpuPass` | the host, one per instance | `stop()` returns |
| `SpcGpuDevice` | the host | `stop()`, or while the plugin holds a retain |
| values handed to `start` / `params_changed` | the host | the call returns |
| a control reply | the host | the call returns |

### Limits

| Limit | Value | Of |
|---|---|---|
| `SPC_PORT_MAX` | 16 | inputs, and outputs, per node |
| `SPC_PLUGIN_PARAM_MAX` | 64 | parameters per node |
| `SPC_PARAM_NAME_MAX` | 64 | a parameter's id, label and group, nul included |
| `SPC_PARAM_STRING_MAX` | 256 | a STRING value, and a parameter's help |
| `SPC_PARAM_TEXT_MAX` | 4096 | a TEXT value |
| `SPC_PARAM_ENUM_MAX` | 32 | options of an ENUM |
| `SPC_PARAM_ENUM_LABEL_MAX` / `SPC_PARAM_ENUM_ID_MAX` | 32 / 48 | an option's label / id |
| `SPC_LIST_SCHEMA_MAX` | 4 | LIST parameters per node |
| `SPC_LIST_COL_MAX` / `SPC_LIST_ROW_MAX` | 8 / 256 | columns / rows of a LIST |
| `SPC_FIELD_NAME_MAX` | 64 | a field's id, nul included |
| `SPC_SCHEMA_MAX_FIELDS` | 48 | fields per schema |
| `SPC_TABLE_MAX_BYTES` | 256 MiB | one table |
| `SPC_JSON_MAX_BYTES` | 16 MiB | one JSON output |
| `SPC_PACKET_MAX_BYTES` | 64 MiB | one packet |
| `SPC_SIGNAL_QUEUE_RECORDS` | 1,048,576 | records the host holds per signal input |
| `SPC_EVENT_LABEL_MAX` / `_DETAIL_MAX` / `_DATA_MAX` | 64 / 192 / 512 | an event's strings, nul included |
| `SPC_TAG_MAX` / `SPC_TAG_NAME_MAX` | 8 / 32 | domain tags per node, and their length |

---

## The Clock

The engine keeps one clock, with two timelines, and every plugin reads the same one. That is what lets data from different sensors, and from different machines, be aligned in time.

- **`host->now_utc_ns(host)`**: disciplined UTC nanoseconds, the timeline of every `timestamp_ns`.
- **`host->now_mono_ns(host)`**: monotonic nanoseconds, which never step back. Use it, or `std::chrono::steady_clock`, for intervals and pacing, never for a timestamp.
- **`host->time(host, &info)`**: both readings in one `SpcTimeInfo`, with the source the clock follows (`sync_source`), whether it is locked to it, and how far the UTC reading may be from it (`est_error_ns`).

A source stamps the moment of capture, when its device reports one. A filter passes its input's timestamp on.

**Steering the clock.** A plugin that reads a time reference, such as a GPS receiver's pulse, feeds corrections to the engine clock:

```cpp
SpcClockCorrection c{};
c.target_utc_ns = pulse_utc_ns;   // what UTC should read...
c.at_mono_ns = pulse_mono_ns;     // ...at this monotonic instant
c.est_error_ns = 1000;            // the correction's error bound
c.sync_source = SPC_SYNC_GPS_PPS;
host->discipline_clock(host, &c);
```

The engine ranks corrections by their source with its own time sources, and the better source holds the clock for as long as it keeps reporting: a GPS pulse outranks NTP and the system clock. Corrections are applied gradually; the clock steps only on first lock or a large error.

---

## Logging

`host->log(host, level, message)` writes a line attributed to the node. The SDK's printf-style macros go through it:

```cpp
s->log = spc::log_context(host);   // in start(): an SpcLogContext that logs through the host
SPC_LOG_INFO(&s->log, "opened %s at %u fps", name, fps);
SPC_LOG_WARN(&s->log, "device busy, retrying");
```

Levels are `SPC_LOG_LEVEL_DEBUG`, `_INFO`, `_WARN` and `_ERROR`, and `SPC_LOG_DEBUG` / `_INFO` / `_WARN` / `_ERROR` match them. The macros format into a 512-byte line. spclib keeps its log sink per thread: call `spc::install_spclib_log_bridge(&s->log)` (`speculor/spclib_log_bridge.h`) at the top of `process()` before calling into it.

---

## GPU Plugins

### How a node uses a GPU

A plugin declares its GPU use in the node info, one `SpcGpuUse`:

| `SpcNodeInfo::gpu` | Builder | Frames it takes | Where it computes |
|---|---|---|---|
| `SPC_GPU_NONE` | | CPU | CPU |
| `SPC_GPU_RECORDED` | `.gpu_compute()` | GPU-resident | in `record_gpu`, into the engine's command buffer, which the engine submits once for a chain of GPU nodes |
| `SPC_GPU_OWN_SUBMIT` | `.gpu_own_submit()` | GPU-resident | records and submits on its own (a fence wait on a dedicated queue); never fused with other nodes |
| `SPC_GPU_OWN_API` | `.gpu_own_api()` | CPU | through an API of its own (ONNX Runtime, CUDA) |

Every value but `NONE` gives the node a GPU switch: the `gpu_enabled` and `gpu_profile` parameters, which the engine adds and owns, a null `host->gpu()` while the node's GPU is off, and a restart on the node's CPU path when it runs out of restarts on the GPU.

A RECORDED or OWN_SUBMIT node gets its GPU-resident frames in `record_gpu`, so a plugin declares either only where it implements `record_gpu`. A build without Vulkan has no `record_gpu`, so it declares no GPU use:

```cpp
SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_bgs", "My BGS", "Analysis/Motion")
        .input("image_in", "Image", SPC_DATA_FRAME, 32, SPC_CONSUME_FIFO)
        .output("mask_out", "Mask", SPC_DATA_FRAME)
        SPC_IF_VULKAN(.gpu_compute())
)
```

Conformance fails a plugin that declares RECORDED or OWN_SUBMIT with no `record_gpu` (`descriptor.gpu_records`).

### The lent device

The engine owns each Vulkan device and lends a node the one it runs on. `host->gpu(host)` returns its `SpcGpuDevice`: the Vulkan instance, physical device and device, the queue families, the enabled extensions, queue locks, a shared lost flag, and `retain` / `release`. Its handles are all null when the host has no Vulkan device for the node. A null `gpu()` means the node's GPU is off, because the user switched it off or the watchdog moved the node to its CPU path: take the CPU path.

A Vulkan plugin uses `spc::gpu::context_from_host(host)` (`gpu/gpu_host.h`), which wraps the lent device as a `VulkanContext`, retained until it is let go, and is null when the node's GPU is off or there is no device. The plugin builds what it needs on the device in `start()`, and destroys it in `stop()` before it lets the device go. A plugin that submits or waits on a queue itself brackets the call with `lock_queue` / `unlock_queue`, since every node on the device shares its queues. One that gets `VK_ERROR_DEVICE_LOST` calls `mark_lost` and makes no further Vulkan call once `is_lost` is set.

### record_gpu

```c
int (*record_gpu)(SpcPluginInstance* instance, SpcStep* step, SpcGpuPass* pass);
```

For a RECORDED node that is on its GPU path, the engine calls `record_gpu` instead of `process()`, on the node's thread, with the same step. `pass->cmd_buffer_handle` is a `VkCommandBuffer` the engine owns.

- **Build the GPU side in `start()`.** `record_gpu` replaces `process()`, so nothing set up lazily in `process()` would ever run.
- **Record into the engine's command buffer**: dispatches and the barriers inside your own work. Never begin, end or submit it; the engine does that once for the whole chain.
- **Outputs come from the engine's ring.** `spc::acquire_ringed_output(pass, port, width, height, stride, format, device_size, staging_size)` gives this frame's slot: point the output binding at `device_buffer`, and set the output frame's `gpu_handle` to the slot's `gpu_handle`, with `SPC_GPU_FLAG_RESIDENT`. When a CPU node reads that output, the engine copies `device_buffer` into the slot's `staging_buffer` in the same submit; `staging_buffer` is null otherwise. The plugin records no download of its own.
- **A CPU-fed input** goes through `spc::acquire_ringed_upload(pass, port, device_size, staging_size)`: copy the CPU frame into `staging_mapped`, then record the upload into `device_buffer`.
- **A GPU-resident input** is read where it is: `spc::gpu::buffer_from_host(host, frame->gpu_handle)` gives the buffer behind its handle.
- **Returns.** A non-zero result cancels the frame for the whole chain, and three in a row move the node to its `process()` path for the rest of the run. `SPC_SKIP` (no frame this time: a stream stall, an exposure in flight) cancels the frame without counting.

```cpp
#ifdef SPC_HAS_VULKAN
static int record_gpu(SpcPluginInstance* instance, SpcStep* step, SpcGpuPass* pass)
{
    auto* s = state(instance);
    SpcHost* host = s->host;
    const SpcData& in_data = step->inputs[0];
    if (in_data.type != SPC_DATA_FRAME || !in_data.frame || !s->gpu_ready) return SPC_FAILED;
    const SpcFrame* in = in_data.frame;

    SpcFrame* out = host->frame(host, 0, in->width, in->height, SPC_PIXEL_FORMAT_GRAY8);
    if (!out) return SPC_FAILED;

    SpcGpuEdgeBuffer ob = spc::acquire_ringed_output(
        pass, 0, out->width, out->height, out->stride, SPC_PIXEL_FORMAT_GRAY8,
        s->pipeline.output_size(), s->pipeline.output_size());
    if (!ob.device_buffer || ob.gpu_handle == 0) {
        host->release_frame(host, out);
        return SPC_FAILED;
    }

    VkBuffer input = VK_NULL_HANDLE;
    if (in->gpu_handle != 0 && (in->gpu_flags & SPC_GPU_FLAG_RESIDENT))
        input = spc::gpu::buffer_from_host(host, in->gpu_handle).buffer;

    // the plugin's own pipeline records its dispatches into the engine's command
    // buffer, writing ob.device_buffer
    auto cmd = static_cast<VkCommandBuffer>(pass->cmd_buffer_handle);
    if (!s->pipeline.record(*s->vk_ctx, cmd, *in, input, ob)) {
        host->release_frame(host, out);
        return SPC_FAILED;
    }

    out->gpu_handle = ob.gpu_handle;
    out->gpu_flags |= SPC_GPU_FLAG_RESIDENT;
    out->timestamp_ns = in->timestamp_ns;
    out->sequence = in->sequence;
    step->outputs[0].type = SPC_DATA_FRAME;
    step->outputs[0].frame = out;
    return SPC_OK;
}
#endif

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,   // the CPU path
    .params_changed = params_changed,
    SPC_IF_VULKAN(.record_gpu = record_gpu,)
)
```

`examples/wmv_bgs` in the examples repository is a complete RECORDED plugin: its pipeline class, its shaders, its CPU path through spclib, and the upload of a CPU-fed input.

### Frames in flight

The engine may run a GPU chain several frames deep (the GPU pipeline depth, a setting of the engine), recording frame N+1 while frame N is still on the GPU. A plugin does not need to know how deep. The engine orders the frames on the GPU, and the ring calls hand each frame its own buffers. What no barrier can order is the CPU, and two calls cover it:

- `spc::acquire_ringed_scratch(pass, key, kind, size)`: a host-visible buffer per frame in flight, for an upload the plugin fills by `memcpy` (`SPC_GPU_SCRATCH_UPLOAD`) or a readback it reads when the same buffer comes round again (`SPC_GPU_SCRATCH_READBACK`).
- `spc::release_after_frame(pass, release, arg)`: calls `release(arg)` once every frame recorded so far has finished on the GPU, for what a frame used and the plugin is done with: a decoder's frame, buffers replaced on a resize.

`pass` stays valid until `stop()` returns, so a plugin may keep it to call `release_after_frame` from `process()`.

### OWN_SUBMIT and OWN_API

An OWN_SUBMIT node gets its GPU-resident input in `record_gpu` too, but records and submits on its own queue, under `lock_queue`, and waits there; it is never fused with its neighbours, so its wait cannot stall a chain. An OWN_API node takes CPU frames and computes through its own API; with a null `gpu()` it takes its CPU path. Both report the GPU time the engine cannot see with `host->gpu_time(host, microseconds)`.

### Building a GPU plugin

```cmake
spc_add_plugin(my_bgs SOURCES my_bgs_plugin.cpp)
target_link_libraries(my_bgs PRIVATE SpeculorSDK::spclib)

if(SPC_VULKAN_FOUND AND SPC_GLSLANG_VALIDATOR)
    spc_enable_gpu(my_bgs)                                 # links the GPU helpers, defines SPC_HAS_VULKAN
    spc_add_gpu_shaders(my_bgs SHADERS my_compute.comp)    # shaders/my_compute.comp -> my_compute_comp.spv.h
    target_sources(my_bgs PRIVATE my_gpu_pipeline.h my_gpu_pipeline.cpp)
endif()
```

`find_package(SpeculorSDK)` sets `SPC_VULKAN_FOUND` and `SPC_GLSLANG_VALIDATOR`: the bundle ships the Vulkan loader and headers and `glslangValidator`. `spc_add_gpu_shaders` expects its shaders in `./shaders/`, and takes `TARGET_ENV vulkan1.2` for shaders that need SPIR-V 1.5. Keep the GPU headers and code under `#ifdef SPC_HAS_VULKAN`, so a build without Vulkan still compiles the CPU path.

`gpu/gpu_pipeline_base.h` holds `GpuPipelineBase`, a base class for compute pipelines. Inside `record_gpu`, `ScopedExternalRecording scope(*this, cmd)` records into the engine's command buffer; `cmd_dispatch_compute`, `cmd_upload_input`, `cmd_download_to_staging` and the `barrier_*` helpers do the common steps. `gpu/gpu_failure_tracker.h` holds a default policy for GPU failures in `process()`: it tears the GPU side down after repeated failures and tells the plugin when to return `SPC_DEVICE_LOST`.

---

## Two-Phase Shutdown

A plugin can block in `process()` on I/O that only an outside event ends: a stalled network stream, a socket with no data, an HTTP request to a server that does not answer. The engine stops a node in two phases so that such a plugin can be stopped in bounded time, and so that teardown never races a step:

| Phase | Slot | When | What the plugin does |
|---|---|---|---|
| 1, abort | `interrupt` (optional) | from another engine thread, before the engine waits for the node's thread to leave the plugin; a `process()` or `record_gpu` may be running | Sets flags and cuts blocking I/O. Frees nothing a step reads. Returns at once |
| 2, teardown | `stop` | after the node's thread has left the plugin | Closes devices, joins threads, frees the instance |

A plugin whose steps cannot block leaves `interrupt` out. One that can block implements it, and ends the blocking call with the means the call offers:

- **FFmpeg**: an `AVIOInterruptCB` that reads the flag `interrupt()` sets. spclib's `video::VideoReader::request_stop()` sets the one it installs.
- **HTTP through spclib**: `spclib::http::RequestOptions::cancel` points at an `std::atomic<bool>`. Setting it ends a request in flight, stalled or not, within about a second, and a request made while it is set is not sent (`spclib/http/http.hpp`, link `SpeculorSDK::spclib_http`).
- **Map tiles**: `spc::map::DownloadQueue::interrupt()` (`spc_map_tiles.h`) ends the download in flight and the queue's idle wait. It sets flags only, so it is safe while `process()` runs. The queue's own `stop()`, called from the plugin's `stop()`, joins its worker.
- **Sockets**: shutting a socket down from another thread does not reliably wake a blocked call on every platform. Wait on the socket with a bounded timeout (`poll`) and check the flag between waits; `examples/udp_scalar_source` does this.

```cpp
static void interrupt(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    s->stopping.store(true, std::memory_order_release);   // read by process() between waits
    s->cancel.store(true);                                 // RequestOptions::cancel of the request in flight
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    if (s->worker.joinable()) s->worker.join();
    delete s;
}
```

Without `interrupt`, a step blocked on I/O holds up the node's stop until the I/O ends or the engine gives up on the node's thread.

---

## The C++ Composition Layer

The examples write the boundary out. The SDK also has a composition layer for C++ plugins, which builds the two lifecycle slots from smaller functions; Speculor's own plugins use it:

```cpp
struct MyState {
    spc::HostServices host;   // the SpcHost, wrapped
    float gain = 1.0f;
};

SPC_PLUGIN(MyState, host)   // state(), create_instance(), destroy_instance(), set_host_services()

static int set_parameter(SpcPluginInstance* inst, const char* name, const SpcParameterDesc* v)
{
    if (std::strcmp(name, "gain") == 0) state(inst)->gain = v->float_val.value;
    return 0;
}

SPC_EXPORT_PLUGIN_SLOTS(
    .info              = info,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .params_changed    = spc::params_by_name<info, set_parameter>,
    .process           = process,
    .set_host_services = set_host_services
)
```

- `SPC_EXPORT_PLUGIN_SLOTS(...)` (`plugin_macros.h`) takes `spc::PluginSlots`, in its declaration order: `info`, `create_instance`, `destroy_instance`, `params_changed`, `param_rules`, `list_options`, `process`, `start`, `stop`, `set_host_services`, `ui_input`, `control_reply`, `record_gpu`, `interrupt`.
- The exported `start(host, values)` is `create_instance`, then `set_host_services`, then `params_changed` with every value, then the slot `start(instance)` (0, or the node does not run). The exported `stop` is the slot `stop(instance)`, then `destroy_instance`. The other slots pass through as they are.
- `spc::params_by_name<info, set_parameter>` calls `set_parameter(inst, name, value)` once per changed parameter, with the value in the C++ carrier `SpcParameterDesc` (`param_carrier.h`). A third template argument receives LIST parameters as rows (`SPC_PLUGIN_LIST_HANDLER`). `SPC_PLUGIN_AUTO_PARAMS` with `SPC_BIND_*` generates `set_parameter` from field bindings.
- `spc::HostServices` (`param_dispatch.h`) wraps the host: `frame`, `table`, `take_signal`, `gpu`, `set_int` / `set_float` / `set_choice` / `set_list`, `set_disabled` / `set_range` / `set_options`, `emit_event`, `control(out, request_id)` and the others. `spc::take_signals(host, info(), handler)` hands every waiting signal block to `handler(port, block)`. `spc::ParamRules` writes a `param_rules` function by parameter name.

---

## CMake Integration

### Project setup

`find_package(SpeculorSDK REQUIRED)` resolves from the extracted bundle and brings in the `SpeculorSDK::` targets. It does not include the helpers: it only puts the bundle's cmake directory on `CMAKE_MODULE_PATH`. Include both:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_plugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SpeculorSDK REQUIRED)
include(PluginHelpers)      # spc_add_plugin(), spc_add_plugin_test(), spc_enable_gpu(), spc_add_gpu_shaders()
include(CompilerWarnings)   # spc_set_warnings(), which spc_add_plugin() calls

spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

Without `PluginHelpers`, `spc_add_plugin` is an unknown command. Without `CompilerWarnings`, the configure fails inside `spc_add_plugin` on `spc_set_warnings`.

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

`spc_add_plugin(my_plugin SOURCES ...)` makes a shared library that:

- links `SpeculorSDK::speculor_sdk` (the C++ conveniences) and the shared plugin helpers, with their include directories;
- defines `SPC_PLUGIN_EXPORT`, so `SPC_EXPORT_PLUGIN` exports `spc_plugin`, and hides every other symbol;
- has no `lib` prefix, since plugins are loaded by name;
- finds the bundled libraries at run time (RPATH on Linux);
- builds with the SDK's warning set;
- lands in `SPC_PLUGIN_OUTPUT_DIR` when you set it.

### Linking libraries

```cmake
target_link_libraries(my_plugin PRIVATE SpeculorSDK::spclib)        # vision: BGS, blobs, flow, tracking, video I/O
target_link_libraries(my_plugin PRIVATE SpeculorSDK::spclib_http)   # spclib::http
target_link_libraries(my_plugin PRIVATE ${OpenCV_LIBS})             # the bundled OpenCV
target_include_directories(my_plugin SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
```

---

## Testing Your Plugin

The bundle ships a test harness in `include/speculor_common/testing/`. It loads a **built** plugin the way the host does, through its exported `spc_plugin`, so it tests the library you ship rather than your functions linked into a test. It uses no test framework: the checks return a report, so they drop into Catch2, GoogleTest or a bare `main`.

| Header | What it gives you |
|---|---|
| `<testing/plugin_under_test.h>` | `PluginUnderTest`: loads the plugin, holds a node's parameter values, and starts, steps, interrupts and stops it |
| `<testing/fake_host.h>` | `FakeHost`: an `SpcHost` with every function set, a frame pool, lent tables, signal inputs, captured logs and a clock you drive |
| `<testing/conformance.h>` | `run_conformance()`: the checks every plugin must pass |

### Conformance

`run_conformance()` checks what the host relies on:

- **The export**: `abi` equals the harness's `SPC_ABI`, and `info`, `start` and `stop` are set.
- **The node info**: no null string, counts within the limits, port and parameter ids unique and not empty, field ids that fit, defaults inside their own ranges, a GPU use that has its `record_gpu` (`descriptor.gpu_records`), and the `.listed()` flag matching `list_options` (`options.listed_match`).
- **The lifecycle, on the engine's threads**: `start()` and `stop()` on an engine thread, twice in a row; `process()` on a node thread with a blank datum of each input's type; `interrupt()` from a third thread during a step, after which the step returns; a restart; each parameter set to its own default; `param_rules`; an empty block on each signal input; a few UI inputs; `list_options` with refresh on a thread of its own. A call that does not return within 30 s fails.
- **Outputs**: a table output comes from `host->table()` (`output.table_is_lent`).
- **Threads**: the rule for each of the 24 host functions (`threads.*`). The host notes the thread every call comes from, so a `frame()` in `start()` or a `log()` from a thread of the plugin's own after `stop()` fails.

A runner takes the plugin's path:

```cpp
#include <testing/conformance.h>
#include <testing/plugin_under_test.h>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin>\n", argv[0]); return 2; }

    spc::testing::PluginUnderTest plugin;
    if (!plugin.load(argv[1])) {
        std::fprintf(stderr, "FAIL: %s\n", plugin.error().c_str());
        return 1;
    }
    const auto report = spc::testing::run_conformance(plugin);
    std::printf("%s\n", report.summary().c_str());   // lists every failure
    return report.ok() ? 0 : 1;
}
```

### Registering it

`spc_add_plugin_test()` builds the runner against the SDK, with the harness on its include path, and links `dl` where `dlopen` needs it. Register one test per plugin, so a failure names the plugin:

```cmake
enable_testing()

spc_add_plugin_test(my_plugin_tests SOURCES conformance_runner.cpp PLUGINS my_plugin)
add_test(NAME conformance.my_plugin COMMAND my_plugin_tests $<TARGET_FILE:my_plugin>)
set_tests_properties(conformance.my_plugin PROPERTIES TIMEOUT 120)
```

Each `PLUGINS` target is built first and passed to the test as `SPC_PLUGIN_<NAME>`, its path. A target that does not exist fails the configure. Run the tests with `ctest --test-dir build --output-on-failure`.

### Beyond conformance

Conformance proves that a plugin loads and keeps the ABI's rules; it cannot tell whether the plugin does its job. A behaviour test drives it through a `FakeHost` and checks what `process()` produces:

```cpp
spc::testing::PluginUnderTest plugin;
if (!plugin.load(SPC_PLUGIN_MY_PLUGIN)) return 1;
spc::testing::FakeHost host;

plugin.create();                             // the node, with its parameter values
host.serve_params(plugin.params());          // the plugin's own set_param calls reach them
host.serve_tables(plugin.descriptor());      // host->table() lends tables of its outputs
plugin.set_host_services(host.services());   // the host start() is handed
plugin.start();

SpcParameterDesc gain{};
plugin.get_parameter("gain", &gain);         // the declaration and its value
gain.float_val.value = 2.0f;
plugin.set_parameter("gain", &gain);         // as the host does: params_changed on the instance

SpcData in[1]{};   // fill the input
SpcData out[1]{};
plugin.process(in, 1, out, 1);               // then check out[0]
plugin.stop();
```

- Hand the host over after `create()` and before `start()`: `start()` gets it, as it gets the engine's.
- `FakeHost` also gives a test `set_time_ns` / `advance_ns` for the clock, `send_signal` for a signal input, `as_input` to lay out a table as an input receives it, `log_lines()`, `control_commands()`, `disable_frames()` to make `frame()` return null, and `lend_gpu` / `switch_gpu_off` for the GPU.
- A plugin that needs hardware still has paths worth testing without it: a scan with nothing attached, a start with no device, and the shutdown sequence. A plugin that blocks for ever in `stop()` once its device is gone reproduces without the device.

---

## Health and the Watchdog

The engine watches every node for hangs and crashes, and treats every instance as replaceable:

- **A crash** in a call into the plugin (a memory fault, an arithmetic fault, an illegal instruction) is caught and logged with the node's name, and the node is restarted with a new instance. An exception that escapes a call is logged the same way and counts as an error.
- **A hang**: a `process()` that does not return within the node's timeout (10 s, or 30 s for sources and GPU nodes, by default) is abandoned. Its thread is left behind, and the node is restarted with a new instance.
- **The restart limit**: after a few restarts (3 by default) the node is disabled, and the nodes that depend on it with it. A GPU node out of restarts carries on on its CPU path instead: its `gpu()` is null from then on.
- **Errors** returned by `process()` back off, from 50 ms up to 5 s. A parameter change retries at once.

What this asks of a plugin:

- **Reach a working state from the parameters alone.** A restart is `start()` on a new instance: open the device again, rebuild what `stop()` released.
- **Keep no unguarded global state.** An abandoned instance may still be running on its thread while its replacement starts.
- **Bound every wait.** Use timeouts on device and network reads. Return `SPC_SKIP` when a wait is a normal drought, and an error for a real failure.
- **Guard your own threads.** The engine contains faults in calls it makes into the plugin, not on threads the plugin starts itself: a fault there ends the process.

---

## Shared Plugin Headers

The bundle ships these in `include/speculor_common/`; `spc_add_plugin()` puts that directory on your include path.

| Header | Purpose |
|---|---|
| `cv_helpers.h` | OpenCV and `SpcFrame`: `frame_to_mat()` / `mat_to_frame()` without a copy, `cv_type_for_format()`, stride-aware copies |
| `gpu/gpu_host.h` | The lent device (`context_from_host`), the buffer behind a GPU handle (`buffer_from_host`), shared buffers |
| `gpu/gpu_pipeline_base.h` | `GpuPipelineBase`, a base for compute pipelines |
| `gpu/gpu_utils.h`, `gpu/vulkan_context.h` | Buffer, staging and pipeline helpers, and the `VulkanContext` a plugin works through |
| `gpu/gpu_failure_tracker.h` | A default policy for GPU failures |
| `spc_clock.h` | `spc::clock::now_utc_ns` / `now_mono_ns` / `get_time` / `discipline`, over the host's clock functions |
| `spc_map_tiles.h` | Web-Mercator map tiles: projection, a memory and disk cache, and `spc::map::DownloadQueue` (needs OpenCV and `SpeculorSDK::spclib_http`) |
| `spc_ui_panel.h`, `spc_ui_hit.h`, `spc_ui_text.h`, `spc_ui_theme.h` | Controls drawn on video frames (buttons, toggles, sliders, dropdowns), their hit testing, outlined text and theme |
| `fps_meter.h` | Delivered frame rate over a sliding window |
| `recording_helpers.h`, `metadata_helpers.h` | File naming for recorders, and JSON metadata as container tags |
| `pocketfft_hdronly.h` | A header-only FFT |
| `domains/adsb/schema.h` | The ADS-B table schema the ADS-B plugins share |
| `testing/...` | The test harness ([Testing Your Plugin](#testing-your-plugin)) |

## SDK Headers

The SDK's headers are in `include/speculor/`:

| Header | Purpose |
|---|---|
| `plugin.h` | The whole boundary, in C: `SpcPlugin` and `SPC_EXPORT_PLUGIN`, `SpcNodeInfo`, `SpcHost`, `SpcStep`, `SpcGpuPass`, the data types, results and limits, `SPC_ABI` |
| `plugin_helpers.h` | The C++ conveniences in one include: the builder, the macros, the helpers below |
| `plugin_macros.h` | `SPC_PLUGIN_DESCRIPTOR`, `SPC_IF_VULKAN`, `SPC_EXPORT_PLUGIN_SLOTS` and the composition layer's macros |
| `descriptor_builder.h` | `spc::DescriptorBuilder` |
| `param_dispatch.h` | `spc::log_context`, `spc::ControlOut`, `spc::HostServices`, the ring call wrappers, `spc::params_by_name`, `spc::ParamRules` |
| `io_helpers.h` | `spc::input_frame` / `input_table` / `input_json` / `input_scalar` and the `set_*_output` helpers |
| `table_helpers.h` | Schema offsets and the typed table accessors |
| `schema_storage.h` | `spc::SchemaStorage`, a schema that owns its fields |
| `plugin_log.h` | `SpcLogContext`, `spc_log` and the `SPC_LOG_*` macros |
| `decimal_helpers.h` | `SpcDecimal` construction and formatting |
| `ring_buffer.h` | A lock-free single-producer, single-consumer ring buffer |
| `spclib_log_bridge.h` | Routes spclib's log to the node's |
| `sdr_params.h`, `sdr_source_helpers.h` | Standard SDR parameter names, and helpers for SDR sources |
| `geo_math.h`, `camera_params.h`, `astro_coords.h` | Geodesy, camera models and astronomical coordinates |
| `version.h` | `SPC_SDK_VERSION` and its parts |

### SDR parameter names

`speculor/sdr_params.h` names the parameters SDR sources share, so a control panel can drive any of them by name:

| Group | Parameters |
|---|---|
| Tuning | `center_freq`, `sample_rate`, `bandwidth`, `ddc1_type` |
| Demodulator | `demod_mode`, `demod_bw`, `demod_freq_offset`, `demod_filter_shift`, `demod_filter_length` |
| Gain | `agc_enabled`, `agc_speed`, `mgc_gain`, `max_agc_gain`, `attenuator`, `preamplifier` |
| Filters | `squelch_level`, `notch_enabled`, `notch_freq`, `notch_bw`, `notch_length`, `noise_blanker`, `adc_nb_threshold` |
| Hardware | `dithering`, `ext_reference`, `inverted`, `channel`, `mw_filter`, `led_mode`, `direct_sampling`, `bias_tee`, `freq_correction` |

A source declares only what its hardware supports; a control panel finds what is there with a control LIST.
