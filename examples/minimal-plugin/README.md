# Minimal plugin example

A frame-in / frame-out filter with one float parameter — the smallest complete
Speculor plugin. It also doubles as the self-containment check for an SDK
bundle: if this builds against an extracted bundle with only CMake + a C++20
compiler, the bundle is good.

## Build

1. Download a bundle from
   [Releases](https://github.com/speculor-app/speculor-sdk-dist/releases/latest)
   and extract it, e.g. to `~/speculor-sdk`.
2. Configure with `CMAKE_PREFIX_PATH` pointing at the extracted folder:

   **Linux** (x64 — use `Linux-aarch64` on arm64)
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

The output is a loadable plugin shared library (`my_plugin.dll` / `my_plugin.so`)
under `build/`. Drop it into the host application's plugin directory to load it.

## What it shows

| Piece | Role |
|---|---|
| `SPC_PLUGIN(MyPluginState, host)` | Generates `state()`, `create_instance()`, `destroy_instance()`, `set_host_services()` |
| `SPC_PLUGIN_DESCRIPTOR(...)` | Metadata, ports, and parameters via `spc::DescriptorBuilder` — pass the builder in directly, there is no `build()` to call |
| `SPC_PLUGIN_AUTO_PARAMS(...)` | Binds the `gain` parameter straight to the struct field |
| `SPC_PLUGIN_VTABLE(...)` | Exports `spc_plugin_vtable`; unlisted slots default to `nullptr` |

`process()` here is a stub — a real filter would scale pixels by `gain`. See the
[Plugin Development Guide](../../docs/plugin-development.md) for reading frames,
tables and signals, GPU compute, and the full SDK reference.
