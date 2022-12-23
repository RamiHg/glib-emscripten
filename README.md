# GLib for Emscripten

This is an Emscripten-compatible port of GLib 2.75.0. The port *mostly* works. However, some parts
that are incompatible with Emscripten are either no-ops, or will fail at runtime. Do not use this in
production until you've thoroughly tested your use-case.

The bulk of the work was done by @kleisauke in their excellent patch
[here](https://gist.github.com/kleisauke/acfa1c09522705efa5eb0541d2d00887).

This branch adds even more fixes and re-enables more components (like gregex).

## Building

First, setup the project: choose an install directory, only build the static version of the library,
reference the Meson [cross-file](emscripten-crossfile.meson), and disable some unnecessary
components.

```sh
meson setup _build --prefix="path/to/install/dir" \
    --cross-file=./emscripten-crossfile.meson \
    --default-library=static \
    --buildtype=release \
    --force-fallback-for=gvdb \
    -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled \
    -Dtests=false -Dglib_assert=false -Dglib_checks=false
```

Then, build and install GLib to your target directory:

```sh
meson install -C _build
```
