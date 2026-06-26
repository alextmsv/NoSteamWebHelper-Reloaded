# Changelog

## 1.1 — 2026-06-25

- Removed dead tray code entirely (ShowTrayMenu, WndProc, TrayThreadProc, g_manualOverride, shellapi.h include). The DLL now runs fully automatic; no tray icon or manual override.
- Updated the auto-disable logic to match current Steam behavior: liveGameProcess check removed, registry RunningAppID + Running flag is the authoritative source.
- Fixed the test to match the updated logic.

## Reloaded

- Fixed current Steam compatibility by turning `umpdc.dll` into a proper proxy that forwards the real Windows `UMPDC` exports through `umpdc_system.dll`.
- Fixed the monitor-thread stack overflow by moving large process snapshots off the stack and onto the heap.
- Limited the DLL logic to the main `steam.exe` process.
- Delayed startup initialization to avoid interfering with Steam's early UI bootstrap.
- Disabled the tray override UI in the compatibility build because that path was unstable on current Steam builds.
