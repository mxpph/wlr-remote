# wl-remote

Remote control your mouse on any Wayland Compositor implementing the
wlr-virtual-pointer-unstable-v1 protocol.

## Building

Building is done with CMake. You will also need the Wayland development headers.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
