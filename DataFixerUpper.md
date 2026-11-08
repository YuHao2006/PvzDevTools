# DataFixerUpper (DFU)

DFU is an offline serialized-save processor. It never loads game classes, PAKs,
graphics, audio, or a game Wasm module. Source belongs to PvzDevTools; pvz_perf
ships only the tool binaries and its thin platform adapters.

- Windows: `pvz_dfu.dll`; game deploys it in `data/`. The standalone converter
  executable is a launcher for the same DLL, with ordinary conversion commands
  and `--dfu input.dat64 output.dat64` for upgrade plus repair.
- Android: `libpvz_dfu.so`, arm64 API 24, static C++ runtime, 16 KiB segment alignment.
- Web: independent wasm32 core in the existing save-tool Worker. The host stages
  replacements before publishing them and terminates the worker before gameplay.

`pvz_dfu_process_v1(input, output, error, capacity)` is a C ABI. Output must be a
new path distinct from input. Returns 1 when an upgraded/repaired output was
written, 0 for a healthy current save, -1 with a bounded error message on failure.
No C++ object or exception crosses the boundary.

Current v5.1 inputs return unchanged without a repair scan. Only v4 inputs run
upgrade and historical repair. Output: v5.1. Known shooter repairs require
an inactive shooting timer and an associated held animation on its completed
last frame; Cob Cannon must also be in its firing state. Growth/arming/instant
plants are excluded. Only the identified timer is re-enabled, without changing
position, RNG or animation phase. The normal game code performs completion.

The processor validates complete records and output roundtrips before writing.
The host replaces the original only after success. DLL/so loading, conversion,
validation and repair are cold paths. Format version 5.1 and its layout stay
unchanged by the DFU deployment architecture.
