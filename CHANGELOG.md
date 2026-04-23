# Changelog

## [Unreleased] — libjmd 0.1.0 · M0 skeleton

### Added
- Public API header `include/libjmd.h` — Visitor API, DOM API,
  allocator hook, scalar / envelope / value types, error reporting.
- `jmd_version()` as the only function with a real body.
- Plain POSIX Makefile with `lib`, `test`, `install`, `uninstall`,
  `clean` targets. Builds both static (`libjmd.a`) and shared
  (`libjmd.so` / `libjmd.dylib`) libraries with SONAME / install_name
  conventions.
- `libjmd.pc` pkg-config file generated from `libjmd.pc.in`.
- GitHub Actions CI for Linux (gcc + clang), macOS (clang), and
  Windows via MSYS2/MinGW64.
- `LIBJMD_API` macro in the public header provides Windows
  `dllexport`/`dllimport` decoration automatically and MSVC-compatible
  ABI. On Unix it expands to a GCC/Clang visibility attribute so
  `-fvisibility=hidden` builds are possible in the future.
- Windows build via MSYS2/MinGW64 produces `libjmd-0.dll` and
  `libjmd.dll.a` (import library). Native MSVC build is explicitly
  deferred until later — consumers can still link MSVC code against
  the MinGW-built DLL because the ABI is compatible.
- `jmd-spec` included as a git submodule at `vendor/jmd-spec/` for
  the shared conformance corpus. Used from M2 onwards.

### Notes
- All parse / serialize entry points return `JMD_ERROR_UNIMPLEMENTED`
  or `NULL` until the real implementation lands in M1 and later.
- Version macros in `include/libjmd.h` and the `VERSION_*` variables
  in `Makefile` must stay in sync. A sync-check is planned for M7.
