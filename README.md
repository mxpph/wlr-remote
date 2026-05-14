# wlr-remote

Remote control your mouse from another device through an encrypted connection.

For when you're too lazy to get up and move your mouse.

## How to run

wlr-remote works on any Wayland compositor implementing the
wlr-virtual-pointer-unstable-v1 protocol. e.g. niri, Sway, Hyprland, etc.
You can see if your compositor supports
it [here](https://wayland.app/protocols/wlr-virtual-pointer-unstable-v1#compositor-support).

Building is done with CMake. You will also need the Wayland development headers.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
