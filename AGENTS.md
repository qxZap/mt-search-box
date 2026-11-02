# AGENTS.md -- Search Box

A native Motor Town plugin: one DLL loaded by
[Plugin Loader](https://github.com/qxZap/mt-plugin-loader). Source is `plugins/search_box.cpp`.
`include/` is a copy of the loader's headers; refresh it from mt-plugin-loader when the
api version changes.

## Build and test

`build.bat` (MSVC 2022 x64) writes `out\`. Install to `MotorTown\Binaries\Win64\plugins\`
with the game closed. There is no test suite: the log `plugins\mt_plugin_<Name>.log` in that
folder is the check, so log what you need to see in game.

## Rules

- The loader owns the ProcessEvent hook and engine discovery. Use `include/mt_plugin_api.h`,
  and never hook a vtable from a plugin.
- `process_event` only on the game thread (`run_on_game_thread`).
- Find fields by reflection (`api->field_offset`) and code by signature (`find_sig`), never
  by address. A missing field or signature logs a line and leaves the game vanilla.
- Hardcoded offsets are the exception and need a `shortcut:` comment naming the limit.
- Mappings come from the loader: `[Mappings] Export=1` in `mt_loader.ini`. After a game
  update recheck every signature and hardcoded offset.
- A plugin rewriting game data affects the machine running the game: single player or the
  host. On someone else's server that server's rules apply.
- Settings live in `Binaries\Win64\<plugin>.ini`, through the loader's ini functions. After
  renaming one, move the old file on start so players keep their settings.

## This plugin

Adds the game's `W_StringSearchBox` to the garage part list, the decal picker and NPC vendors, and filters through `ListView.SetListItems`. Filtered-out items stay in a collapsed second ListView, because a ListView is what keeps its items alive through garbage collection. To add a screen, add a line to `kinds[]`. Item key offsets (`itemKey`) come from the code filling the items; recheck after an update. Engine lists get fuel checkboxes; the fuel is read from the engine asset, loaded through its soft reference.
