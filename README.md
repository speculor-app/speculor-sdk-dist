# speculor-sdk-dist

Public distribution for the **Speculor plugin SDK** — the development bundle and
API reference for building plugins against the Speculor engine.

> The SDK *source* lives in a private repository. This repo is the public
> distribution point: the downloadable development bundle (attached to
> [Releases](../../releases)) plus the API reference below. The engine and the
> Qt6 app ship as commercial binaries — see <https://speculor.app/license>.

## Get the SDK

Download the latest development bundle from the [**Releases**](../../releases)
page and extract it. The bundle contains the public C ABI headers, the build
setup, and everything needed to compile a plugin against a matching engine
release.

## API reference

| Header | What it covers |
|---|---|
| `plugin_api.h` | The plugin C ABI — entry points, vtable layout, ABI version guard. |
| `data_types.h` | Typed packet kinds — frames, tables, signals, scalars, GPU handles. |
| `gpu_pipeline_base.h` | GPU plugin base — `record_gpu()` helpers, command-buffer plumbing. |

The ABI is versioned; build against the bundle that matches your target engine
release. Check the release notes for ABI changes between cycles.

## Examples

Minimal, buildable example plugins (sources, filters, a Vulkan compute shader,
an FFT spectrometer) live in
[**speculor-plugin-examples**](https://github.com/speculor-app/speculor-plugin-examples).

## Links

- Site · <https://speculor.app>
- Docs · <https://speculor.app/docs>
- Downloads · <https://speculor.app/downloads>
- Community · <https://speculor.app/community>
