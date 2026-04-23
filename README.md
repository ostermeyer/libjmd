# libjmd

Copyright © 2026 Andreas Ostermeyer.

A language-agnostic C library for parsing and serializing JMD documents.
Part of the JMD ecosystem — see [jmd-spec](https://github.com/ostermeyer/jmd-spec)
for the format specification.

## Status

**Milestone 0 — installable skeleton.** The full public API is declared
in [`include/libjmd.h`](include/libjmd.h); real parsing and serializing
land in later milestones. All parse / serialize entry points currently
return `JMD_ERROR_UNIMPLEMENTED`.

## API levels

libjmd offers two entry points that share a single internal parser:

- **Visitor / Event API** (`jmd_parse`, `jmd_parse_ex`). Streaming,
  zero allocation in the hot path. Pointers delivered to callbacks are
  valid only during the callback. Consumers copy what they want to keep.
  Best for pipelines and database extensions.

- **DOM API** (`jmd_parse_dom`, `jmd_parse_dom_ex`). Builds an owned
  tree that you can hold onto. The envelope owns all its strings; a
  single `jmd_envelope_free` releases everything.

Both entry points accept an optional `jmd_allocator_t` so the host
(e.g. Postgres with `palloc`/`pfree`) can take over internal buffer
management. Pass `NULL` for the libc default.

## Build

Requires a C11 compiler and GNU Make. No external runtime dependencies
beyond libc.

```
make
make test
make install PREFIX=/usr/local
```

`make install` installs the shared and static libraries, the public
header, and a `libjmd.pc` file for pkg-config:

```
pkg-config --cflags --libs libjmd
```

Standard `DESTDIR` staging is supported for packaging.

### Platforms

- **Linux** and **macOS** are first-class targets: build and install
  with the toolchain your distribution ships (`gcc`, `clang`, `make`).
  CI covers both on every push.
- **Windows** is supported via **MSYS2/MinGW64**: install MSYS2, then
  `pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf` and
  run the same `make && make install` from a MinGW64 shell. The
  library builds as `libjmd-0.dll` with `libjmd.dll.a` as the import
  library.
- **MSVC compatibility** is guaranteed at the ABI level: the public
  header uses `__declspec(dllimport)` when consumed by a non-MinGW
  Windows compiler, so MSVC callers can link against the DLL built
  by MSYS2 without header changes. A native MSVC build script is not
  provided yet — MSVC users bring their own, or wait for a later
  release.

## Conformance

The JMD conformance corpus lives in the `jmd-spec` repository, included
here as a git submodule under `vendor/jmd-spec/`. Run `git submodule
update --init` after cloning. Conformance tests will begin landing in M2.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
