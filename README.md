# GLib for Emscripten

This is an Emscripten-compatible port of GLib 2.75.0. The port *mostly* works. However, some parts
that are incompatible with Emscripten are either no-ops, or will fail at runtime. Do not use this in
production until you've thoroughly tested your use-case.

The bulk of the work was done by @kleisauke in their excellent patch
[here](https://gist.github.com/kleisauke/acfa1c09522705efa5eb0541d2d00887).

This branch adds even more fixes and re-enables more components (like gregex).

## Prerequisites

It is helpful to have a common installation prefix for Emscripten libraries. You can then set
`PKG_CONFIG_PATH` to point to that directory's `lib/pkgconfig`. Packages can then reference each
other seamlessly during compilation.

The build instructions below install Emscripten libraries in `$HOME/emlib`.

> The build instructions also enable `WASM_BIGINT` by default. Remove references to `-sWASM_BIGINT`
> if you do not want to have it enabled.

### PCRE2

```sh
git clone https://github.com/PCRE2Project/pcre2.git --branch pcre2-10.41
cd pcre2
./autogen.sh
emconfigure ./configure --prefix="$HOME/emlib" CFLAGS="-pthread -O3" LDFLAGS="-pthread -O3 -sWASM_BIGINT" --disable-shared
make -j$(nproc) install
```

### zlib, libffi

TODO: Write simple instructions once the zlib fork is ready. For now, just follow the instructions
in @kleisauke's gist linked above.

## Building

First, setup the project. Use the `$HOME/emlib` directory that we've used to install dependencies as
both the target directory, and the `pkgconfig` path. Then, only build the static version of the
library, reference the Meson [cross-file](emscripten-crossfile.meson), and disable some unnecessary
components.

```sh
export EM_PKG_CONFIG_PATH="$HOME/emlib/lib/pkconfig"
CFLAGS="pthread -O3" LDFLAGS="-pthread -O3 -sWASM_BIGINT" meson setup _build \
    --prefix="HOME/emlib" \
    --cross-file=./emscripten-crossfile.meson \
    --default-library=static \
    --buildtype=release \
    --force-fallback-for=gvdb \
    -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled \
    -Dtests=false -Dglib_assert=false -Dglib_checks=false
```

Finally, build and install GLib to your target directory:

```sh
meson install -C _build
```
