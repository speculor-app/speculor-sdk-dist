# Third-Party Notices — Speculor SDK

The Speculor SDK binary distribution incorporates or ships alongside the
following third-party software. Each component listed below is governed by
its own license terms; nothing in `LICENSE` modifies those terms.

The SDK builds an **LGPL-only** edition of FFmpeg: `--enable-gpl`,
`--enable-libx264`, `--enable-libx265`, and `--enable-nonfree` are deliberately
not used.

Plugin authors who develop closed-source or non-Apache plugins against the
SDK should reproduce these notices in any binary distribution of a plugin
that statically links the SDK's helper libraries or that loads at runtime
alongside FFmpeg/Qt/OpenCV/etc. as shipped with the Speculor host. A plugin
that only `#include`s the public C-ABI header in `sdk/include` and does not
link any SDK helper library does **not** transitively redistribute these
third-party components and does not need to reproduce these notices.

## Summary

| Component | Version | License | SPDX |
|---|---|---|---|
| FFmpeg | 7.1.1 | GNU Lesser General Public License v2.1 or later | LGPL-2.1-or-later |
| nv-codec-headers | n12.2.72.0 | MIT License | MIT |
| OpenCV | 4.x | Apache License 2.0 | Apache-2.0 |
| Eigen | 3.x | Mozilla Public License 2.0 | MPL-2.0 |
| libjpeg-turbo | 3.x | BSD 3-Clause + IJG + zlib | BSD-3-Clause |
| libcurl | recent | curl license (MIT/X derivative) | curl |
| Vulkan-Headers | recent | Apache License 2.0 OR MIT | Apache-2.0 OR MIT |
| Vulkan-Loader | recent | Apache License 2.0 | Apache-2.0 |
| glslang | recent | BSD 3-Clause / Apache License 2.0 (mixed) | BSD-3-Clause AND Apache-2.0 |
| oneTBB | recent | Apache License 2.0 | Apache-2.0 |
| nlohmann/json | 3.11.3 | MIT License | MIT |

Each component is identified above by its SPDX license identifier. The full
license text for every component is distributed inside the corresponding
upstream project and applies unmodified. For a copy of any component's full
license text, contact <contact@speculor.app>.
