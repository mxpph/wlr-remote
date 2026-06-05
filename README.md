# wlr-remote

Remote control your mouse from another device through an encrypted connection
with low latency.

For when you're too lazy to get up and move your mouse.

Find the [Android app](https://github.com/mxpph/wlr-remote-control) here.

[![Demo video](https://github.com/user-attachments/assets/95add94a-ad42-40bd-aaf8-5ab7ff95e5d5)](https://www.youtube.com/watch?v=t-NXUtKe0Us)

## How to run

wlr-remote works on any Wayland compositor implementing the
wlr-virtual-pointer-unstable-v1 protocol e.g. niri, Sway, Hyprland, etc. You
can see if your compositor supports it
[here](https://wayland.app/protocols/wlr-virtual-pointer-unstable-v1#compositor-support).

Building is done with CMake. You will also need the Wayland development headers.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Usage

Run the `wlr-remote` executable, and specify a password. Optionally, you may
pass a port number to use, e.g. `./wlr-remote 12345`. The default port is 37096.
There is a `test-client` executable to check that things are working.

To actually connect to the client from your phone, see the [Android client
repository](https://github.com/mxpph/wlr-remote-control). I don't own any Apple
devices, so I can't make an iOS client, sorry.

## TODOs

- Scrolling support
- `libevdev` backend for systems that dont support the virtual pointer protocol
- Keyboard input (either virtual keyboard protocol or `libevdev`)
- Multi-touch support (requires `libevdev` backend)

Read more about the development [here](https://xgilli.me/projects/linux-remote-mouse).
