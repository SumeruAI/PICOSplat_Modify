# PICOSplat_Modify

This repository is an unofficial modified fork of PICO's public Unreal Engine plugin repository:

- Upstream repository: https://github.com/Pico-Developer/splat_unreal
- Open-source components repository: https://github.com/Pico-Developer/splat

## Status

This repository is not an official PICO release.

It contains local modifications built on top of PICO's public `splat_unreal` source tree for Unreal Engine.

## Licensing and Usage Notes

This repository does not declare a new replacement license for the full plugin.

- Use of Unreal Engine remains governed by the Unreal Engine End User License Agreement: https://www.unrealengine.com/eula
- The upstream `splat_unreal` repository's `LICENSE.md` points to the Unreal Engine EULA rather than to a standard open-source license for the full plugin.
- Open-source third-party portions under `Source/ThirdParty/splat` and `Source/ThirdParty/Shaders/Public` retain their original MIT licensing and attribution.

Unless PICO states otherwise, treat this repository as a modified public fork of the official upstream source, not as a separately relicensed standalone open-source plugin.

## What This Repository Changes

This fork contains project-specific changes on top of the upstream plugin, such as runtime loading, async helpers, blueprint-facing utilities, and related rendering/runtime changes.

These changes are provided for collaboration and reference in the Unreal Engine ecosystem.

## Distribution Guidance

If you redistribute or fork this repository:

- Keep upstream PICO copyright notices intact.
- Keep third-party license files intact.
- Do not remove or override the Unreal Engine EULA reference used by upstream for the full plugin repository.
- Clearly identify your version as an unofficial fork or modified version.
- Do not imply endorsement by or affiliation with PICO.

## Trademarks

PICO, Unreal, and Unreal Engine are trademarks of their respective owners.

This repository is provided only as a modified fork for developers already working within the Unreal Engine ecosystem.