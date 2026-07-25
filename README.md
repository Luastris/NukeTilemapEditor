# NukeTilemapEditor

The EDITOR-ONLY companion of [NukeTilemap](https://github.com/Luastris/NukeTilemap) for
[NukeEngine](https://github.com/Luastris/NukeEngine-Eco): it owns the tooling for the
file types the runtime module registers, keeping the editor core format-blind — asset
types get their own per-file editors through the engine's `RegisterAssetEditor` registry,
never hardcode.

## The .nutile tile set editor

Double-clicking a `.nutile` in the browser routes here; each file opens its own window
(floating/dockable, drags out to a native OS window) with full CRUD:

- texture + atlas grid setup,
- tile list (add/remove), per-tile id / name / walkability / flags,
- a CLICKABLE ATLAS — toggle cells on the selected tile directly on the image,
- Save / Revert, undo, dirty-state guard.

Saving re-parses the loaded tile set IN PLACE — live tilemaps in the open world rebake
immediately.

## Packaging

This module imports NukeImGui, so game packaging auto-excludes it from dists;
`editorTool()` keeps it always-on in the editor host (it is never listed per-project).

## Building

Part of the [NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco) superbuild, or
standalone: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` +
`cmake --build build --config Debug` (needs `VCPKG_ROOT`; engine + NukeTilemap first).
The build copies `NukeTilemapEditor.dll` into the runtime `modules/` folder.
