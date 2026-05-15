# libjmd — C Conventions

This document fixes the coding conventions for **libjmd** and any
JMD-adjacent C code in the ecosystem (later: JMDB, the Postgres
extension, and the CPython glue layer that replaces `jmd-impl`'s
current `_cparser.c` / `_cserializer.c`).

Two layers of discipline apply. The first (§1–§3) is hard — the code
must comply or it will not work correctly in all target hosts. The
second (§4–§7) is stylistic — it keeps the codebase internally
consistent so contributors read one idiom, not three.

---

## §1 API compatibility with PostgreSQL

libjmd is not itself a Postgres extension, but its public header must
compile cleanly **after** `#include "postgres.h"` so JMDB (and any
future in-process consumer) can use both. This constrains what the
public header (`include/libjmd.h`) may say, but leaves internal `.c`
files largely free.

### Windows / MSVC compatibility (§1a)

The public header is MSVC-compatible at the ABI level. Every exported
function declaration carries the `LIBJMD_API` macro, which expands to:

- `__declspec(dllexport)` when building libjmd on Windows
  (`LIBJMD_BUILDING_DLL` defined by the Makefile),
- `__declspec(dllimport)` when a Windows consumer includes the header,
- `__attribute__((visibility("default")))` on GCC/Clang,
- nothing at all on older compilers.

**New public functions must be declared with `LIBJMD_API`.** A declaration
without it will link on Linux/macOS but fail to resolve at load time on
Windows MSVC consumers — a quiet bug we'd rather not ship.

### Must-follow rules for the public header

- **Prefix every identifier with `jmd_` (types) or `JMD_`
  (macros / enums / constants).** No bare `parse`, `Mode`, `ERROR_OK`
  in the header. Postgres uses many short names (`List`, `Node`,
  `Oid`, `Datum`, `bool`, `TupleDesc`), and collisions are silent
  compile breakage or — worse — silent semantic breakage.
- **Do not use `bool`, `true`, `false` in the public header.**
  Postgres defines `bool` as `char`; `<stdbool.h>` defines it as
  `_Bool`. Including both produces a redefinition warning at best and
  ABI divergence at worst. Use `int` with the contract "non-zero =
  true, zero = false".
- **Do not define common uppercase macros** (`TRUE`, `FALSE`, `MIN`,
  `MAX`, `Min`, `Max`, `ABS`, `NELEMS`). Postgres owns them.
- **No `static` storage in the header.** A header may only declare,
  not define, mutable state.
- **Always wrap the header contents in `#ifdef __cplusplus extern "C"`
  so it is usable from C++ hosts.**
- **Include only `<stddef.h>` and `<stdint.h>` in the public header.**
  Anything else (stdio, stdlib, string.h) belongs in the `.c` files.

### Memory: the allocator hook is the only bridge

libjmd internally uses its own allocations, but every public entry
point that allocates accepts a `jmd_allocator_t`. The host threads
its own allocator (Postgres: `palloc`/`repalloc`/`pfree`) through
this struct. libjmd **must not** call `malloc` directly inside any
code path that is reachable from a public entry point — doing so
would silently bypass the host allocator and produce memory that
leaks across `ereport` (Postgres) or across caller-controlled arenas
(any other host with a region-based memory regime).

The libc default is implemented as a tiny adapter in
`src/alloc.c` that wraps `malloc`/`realloc`/`free` in a
`jmd_allocator_t`. All internal code goes through the allocator. If
you find yourself writing `malloc` anywhere in `src/*.c`, you're
introducing a bug.

### Error handling: never longjmp, exit, or abort

Postgres translates its own errors via `ereport(ERROR, …)`, which
performs a `longjmp`. If libjmd were to call `exit()`, `abort()`,
`assert()` with abort semantics, or its own `longjmp` from inside
a parse, it would kill the Postgres backend. Therefore:

- **All errors are reported via the `on_parse_error` visitor callback
  and/or a negative return code.** JMDB's callback turns them into
  `ereport(ERROR, …)`.
- **No `assert()` in release builds.** We use a custom
  `JMD_ASSERT(cond)` macro that compiles to nothing when
  `NDEBUG` is defined. Assertions are development tools, not error
  handling.
- **No `exit()` or `abort()` anywhere.** Allocation failure is a
  `JMD_ERROR_MEMORY` return, not a crash.
- **No signal handlers, no alarm, no threads with shared state.**
  libjmd is thread-safe at the function level (two parses in
  parallel with separate allocators and visitors are fine), but it
  does not own global infrastructure.

---

## §2 Naming

- **Functions and variables**: `snake_case`.
- **Public functions**: `jmd_` prefix. Example: `jmd_parse`,
  `jmd_envelope_free`.
- **Public types**: `jmd_` prefix, `_t` suffix. Example:
  `jmd_visitor_t`, `jmd_allocator_t`.
- **Public enum constants**: `JMD_` prefix, uppercase.
  Example: `JMD_MODE_DATA`, `JMD_ERROR_PARSE`.
- **Public macros**: `LIBJMD_` or `JMD_` prefix, uppercase.
  Example: `LIBJMD_VERSION_STRING`.
- **Internal (file-scope) types and functions**: no `jmd_` prefix
  required, but must be `static`. Example: `KeyCacheEntry`,
  `intern_key()`.
- **Struct tag naming**: typedef'd structs use PascalCase inside the
  file (e.g. `JMDLine`, `LineArray`) for brevity at the use site;
  the typedef is the authoritative name.

---

## §3 Memory discipline (recap — the single most important rule)

Every allocation in libjmd code goes through the active
`jmd_allocator_t`. This is not negotiable. If you need a helper,
write it to accept the allocator as an argument. If you find
yourself wanting a "small temporary buffer, just this once",
allocate it via the allocator — the overhead is zero and the
correctness is absolute.

---

## §4 Formatting

We follow the style that `jmd-impl`'s `_cparser.c` and
`_cserializer.c` established, because that is the code we are
porting. Unless specified otherwise here, that file is the
ground-truth example.

- **Indent: 4 spaces.** No tabs. Editor-agnostic.
- **Line length: 80 columns soft, 100 hard.** Break long call sites
  by aligning arguments at the opening paren.
- **Function definitions**: return type on its own line, then
  function name and parameter list, then opening brace on a new
  line. Example:

  ```c
  static int
  parse_scalar(const char *raw, size_t len, jmd_scalar_t *out)
  {
      ...
  }
  ```

- **Control blocks** (`if`, `for`, `while`, `switch`): opening brace
  on the same line as the keyword. Example:

  ```c
  if (len == 0) {
      return JMD_ERROR_PARSE;
  }
  ```

- **Always brace single-statement blocks.** No `if (x) return;`
  one-liners — write `if (x) { return; }`. This prevents a class of
  merge-time bugs and reads uniformly.
- **Spacing**: one space after `if`, `for`, `while`, `switch`, `return`;
  no space between function name and opening paren;
  spaces around binary operators.
- **Comments**: always `/* … */`, never `//`. The latter is fine in
  modern C but we stay traditional for consistency with Postgres
  source and to keep the code friendly to legacy toolchains.
- **Section banners**: long files use banner comments to introduce
  logical sections. Example:

  ```c
  /* ------------------------------------------------------------ */
  /* Scalar parsing                                               */
  /* ------------------------------------------------------------ */
  ```

- **No trailing whitespace.** No tabs. Every file ends with a
  single newline.

---

## §5 Include order (`.c` files)

From top to bottom of the file:

1. The project's own header for this translation unit (if any), to
   catch header self-sufficiency bugs.
2. `#include "libjmd.h"` (the public header).
3. Internal project headers (`src/internal.h`, when introduced).
4. System headers, sorted alphabetically: `<ctype.h>`, `<errno.h>`,
   `<stdint.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`, …

Public header (`include/libjmd.h`) contains only `<stddef.h>` and
`<stdint.h>`. See §1.

---

## §6 Commenting — the long rule

**libjmd is a reference implementation of the JMD format.** Anyone
who wants to understand JMD-parsing at a level of detail that the
spec doesn't spell out reads this code. That imposes a
disproportionate documentation duty on us — the code must teach,
not only execute.

We therefore comment generously. The rules that follow are not
about comment *volume* but about *targeted coverage* of the places
a reader most needs help.

### File header

Every `.c` and `.h` file starts with a block comment:

```c
/*
 * <filename> — <one-line purpose>.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * <2–6 lines of narrative: what this file owns, who calls it,
 *  what invariants it preserves. If the code is ported from
 *  jmd-impl, name the source file and the relevant line range.
 *  If a particular spec section drives the logic, cite it.>
 */
```

### Section banners

Every logical region (a type, a group of related helpers, a
state-machine phase) opens with a three-line banner comment (see
§4). A banner without a one-line subtitle is not allowed — if you
can't summarize the section in one line, split it.

### Function-level comments

Every non-trivial function (i.e. anything beyond a one-line
accessor) has a block comment above its definition describing:

- **Purpose** in one sentence.
- **Inputs**: what each non-obvious parameter means, including
  ownership (borrowed / owned / transferred).
- **Output**: return value semantics. For pointer returns,
  ownership and lifetime.
- **Invariants and preconditions** the caller must satisfy.
- **Spec reference** (if the function implements a specific
  spec section), e.g. `/* Implements jmd-spec §8.6a depth-qualified
  items. */`.

Trivial accessors (`jmd_value_kind`, `jmd_envelope_mode`) don't
need a block — the one-line doc in the public header suffices.

### Inline comments

Inline comments explain **why**, not what. The reader can see that
`h ^= (unsigned char)s[i]` is an XOR; they cannot see that this is
part of FNV-1a-32 unless we tell them. Use inline comments at
these points:

- **Algorithms by name**: `/* FNV-1a 32 */` before a hash.
- **Invariants being established or relied upon**: `/* scope is
   root again; emit SCOPE_RESET */`.
- **Ownership transfers**: `/* takes ownership of `buf`; caller
   must not free */`.
- **Subtle spec requirements**: `/* spec §7.2a: blank line
   closes all open scopes, not just the deepest */`.
- **Intentional non-obvious choices**: `/* we allow trailing
   whitespace here because LLMs routinely emit it */`.
- **Port annotations**: `/* ported from _cparser.c:412-468 */`.
- **Non-local effects**: `/* mutates `v` via allocator; fresh
   pointer returned */`.

Avoid what-not-why inline comments. `/* increment i */ i++;` is
noise. The test is: can a competent C reader see what the line
does? If yes, don't comment what. Comment why.

### TODO / FIXME / XXX

Use `TODO(milestone): …` with the milestone that will address it,
so they can be grep'd: `grep 'TODO(M' src/*.c`. Example:

```c
/* TODO(M2): support depth-qualified items per spec §8.6a */
```

`FIXME` is reserved for bugs discovered mid-development that are
being explicitly deferred. `XXX` is reserved for "this works but I
don't like it, revisit when we have more context".

### Commit-as-documentation

When a non-obvious implementation choice is made, the commit
message records *why*. The code comment records *what the reader
needs to know to understand the code in front of them*. The two
are not duplicates. A commit message is read once during review;
a comment is read every time someone opens the file.

---

## §7 Tests

- Every new public function gets at least one test by the milestone
  in which it becomes real.
- Conformance tests consume the corpus at `vendor/jmd-spec/conformance/`.
- Unit tests live under `tests/` and use a minimal custom runner
  (no external dependency). The M0 `tests/test_link.c` shows the
  pattern.
- Fuzzing harness (libFuzzer) lands in M7 and runs nightly in CI.

---

## §8 Versioning

- Semantic versioning.
- `LIBJMD_VERSION_*` macros in `include/libjmd.h` are the source of
  truth. The corresponding `VERSION_*` variables in `Makefile` must
  stay in sync.
- ABI breaks require a major-version bump. During 0.y.z any change
  may break ABI and callers must recompile.

---

## §9 Fallback style: PostgreSQL coding conventions

Where these conventions are silent on a formatting detail, follow
PostgreSQL's source-code conventions
(https://www.postgresql.org/docs/current/source-format.html).
`pgindent` over `src/`, `include/`, and `tests/` should produce a
no-op diff once a slice has settled.

The rules that matter in practice and that PostgreSQL settles for
us:

- **Indentation:** hard tabs, displayed at width 4. The repo
  ships a `.editorconfig` that pins this for `.c`, `.h`, and
  `Makefile`; the equivalent Sublime Text user-level settings live
  under `Packages/User/{C,C++,Makefile}.sublime-settings`.
- **Line width:** 80 columns soft, 100 hard. Wrap long argument
  lists at function-call boundaries, not mid-expression.
- **Braces:** Allman for function definitions (brace on its own
  line), K&R for control flow (brace on the `if`/`while`/`for`
  line). Single-statement bodies still get braces.
- **Comment style:** `/* ... */` everywhere — even single-line.
  Sentence-shaped with terminal punctuation. `//` is reserved for
  temporary debug comments that must not survive a commit.
- **Include order in `.c` files:** see §5.

The few places where libjmd deliberately diverges from PostgreSQL:

- **Header guards:** `LIBJMD_<MODULE>_H` (project-prefix style).
  PostgreSQL uses `PG_<MODULE>_H` because that prefix is its own.
- **Public identifiers:** `jmd_` / `JMD_` prefix mandatory (§1).
  PostgreSQL uses no project prefix at all because there is only
  one project in its source tree.

---

*This document evolves with the codebase. Amend it in the same
commit as the behavior that motivates the amendment.*
