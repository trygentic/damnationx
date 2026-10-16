# Notices and modification history

This experimental HUD module is derived from the Diablo3D / DevilutionX HUD and retains the inherited Sustainable Use License v1.0. See [LICENSE.md](LICENSE.md) for the complete terms. Existing licensing, copyright, attribution and contributor notices remain applicable.

Modifications by **Douglas Pan**, with AI-assisted refactoring and documentation, extract HUD state and host boundaries for standalone use. This attribution covers those modifications; it does not reassign authorship of DevilutionX, Diasurgical, Diablo, Dawn, SDL or inherited contributions.

Source lineage for this extraction: integration baseline `24080e8`, retaining HUD R4 work from `5af5d13`. These identifiers describe provenance, not compatibility certification with another repository or renderer. The host's original commit history and notices should be retained alongside this module.

DevilutionX is maintained by Diasurgical. Diablo and the original game data remain the work and trademarks of Blizzard Entertainment; inherited notices include Blizzard Entertainment 1996–2001. This module does not claim affiliation, endorsement or certification by Blizzard Entertainment or GOG.com.

This core-only package contains portable HUD headers, tests, a headless example and documentation. Native host adapters and the optional theme component are excluded. It includes no proprietary game data, MPQs, artwork, fonts, HD art pack, Warrior/model data, world renderer, camera code, terrain, exterior or save files. Code licensing does not grant rights to separately supplied assets.

The standalone core has no Dawn or SDL dependency. Host integrations retain their own dependency notices. A future WebGPU adapter may consume Dawn, whose pinned upstream license is BSD-3-Clause, copyright 2017–2023 The Dawn & Tint Authors. That planned dependency is not linked or redistributed by the core. The core includes no Dawn files or linked dependency. See [BUILD-CORE.md](docs/BUILD-CORE.md) for the included build entry point.

This is an experimental extraction. A build or test of the standalone core does not certify the native adapter, WebGPU, all platforms, physical input, high DPI or public-target integration.
