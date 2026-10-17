# Palworld VR Injector (workspace)

LD_PRELOAD-based VR injection for Palworld under Wine/Proton: a virtual
Oculus-style display adapter (2448×1236 per eye @ 90 Hz) is injected via a
DXGI hook, and rendered frames are routed to an OpenXR/Vulkan compositor.

## Layout

- `stub.c` — LD_PRELOAD entry point, process detection, logging
- `hook/dxgi_hook.cpp` — Wine kernel32 shim resolution, virtual adapter injection
- `hook/openxr_compositor.cpp` — OpenXR session + stereo swap chains (WIP)
- `Makefile` — builds the combined `libpalworldvr.so` (`make all`)
- `PalworldVrInjector/`, `tests/PidTester/` — .NET injector stubs (Windows-side experiments)

## Status (2026-10-17)

`make all` fails compiling `hook/openxr_compositor.cpp`. The five
compile-blocking defects are tracked in issues #1–#5.
