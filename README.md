# CardputerZero StreamPlayer

LVGL-based stream player app for M5Stack Cardputer Zero.

## Repository Layout

- `main/src/` - application, keyboard input, and media controller source code
- `main/include/` - application headers
- `main/ui/` - generated LVGL UI sources and image assets
- `fonts/` - runtime CJK font packaged with the app
- `*.sh` - deployment and debug helper scripts
- `SConstruct` and `main/SConstruct` - SCons build scripts
- `config_defaults.mk` and `setup.ini` - default build and project configuration

Generated build outputs, toolchain sysroots, and runtime poster caches are intentionally ignored by git.

