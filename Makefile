# SPDX-License-Identifier: Apache-2.0
#
# libjmd — plain POSIX Make build.
#
# Public targets:
#   all (default)  Build static + shared library and the pkg-config file.
#   lib            Static + shared library only.
#   test           Build and run the linker smoke test.
#   install        Stage into $(DESTDIR)$(PREFIX).
#   uninstall      Remove from $(DESTDIR)$(PREFIX).
#   clean          Remove build artifacts.
#
# Override variables on the command line:
#   PREFIX=/usr/local   install prefix
#   DESTDIR=            staging root (empty by default)
#   CC=cc               C compiler
#   CFLAGS=             extra compile flags (appended to defaults)
#   LDFLAGS=            extra link flags

PREFIX ?= /usr/local
DESTDIR ?=
CC      ?= cc
AR      ?= ar

# Keep in sync with LIBJMD_VERSION_* in include/libjmd.h.
VERSION_MAJOR := 0
VERSION_MINOR := 1
VERSION_PATCH := 0
VERSION       := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
        -Wmissing-prototypes -Wno-unused-parameter
OPT  ?= -O2
STD  := -std=c11

UNAME_S := $(shell uname -s)

# Platform-specific shared-library setup.
#
# Three targets: macOS (.dylib with install_name), Linux (.so with SONAME),
# Windows via MinGW/MSYS2 (.dll with import library). The LIBJMD_API macro
# in include/libjmd.h handles the Windows dllexport/dllimport dance; we
# just need to define LIBJMD_BUILDING_DLL while compiling the library so
# it picks the export branch.
ifeq ($(UNAME_S),Darwin)
    # macOS: shared library is .dylib, install_name embeds the install path
    # so consumers don't need -rpath at link time.
    SOEXT     := dylib
    SHLIB     := libjmd.$(VERSION).$(SOEXT)
    SHLINK1   := libjmd.$(VERSION_MAJOR).$(SOEXT)
    SHLINK2   := libjmd.$(SOEXT)
    SHFLAGS   := -dynamiclib \
                 -install_name $(PREFIX)/lib/$(SHLINK1) \
                 -compatibility_version $(VERSION_MAJOR).$(VERSION_MINOR) \
                 -current_version $(VERSION)
    IMPLIB    :=
    PIC_FLAG  := -fPIC
    IS_WINDOWS := 0
else ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),)
    # Windows (MinGW/MSYS2/Cygwin): shared library is .dll, paired with
    # an import library (.dll.a) that consumers link against. There is
    # no SONAME scheme — we version the DLL by embedding the major
    # number in its filename (libjmd-0.dll). -fPIC is the default on
    # Windows and emits a warning if passed explicitly, so we skip it.
    SOEXT     := dll
    SHLIB     := libjmd-$(VERSION_MAJOR).$(SOEXT)
    SHLINK1   :=
    SHLINK2   :=
    IMPLIB    := libjmd.dll.a
    SHFLAGS   := -shared -Wl,--out-implib,$(IMPLIB) \
                 -Wl,--export-all-symbols \
                 -Wl,--enable-auto-import
    PIC_FLAG  :=
    IS_WINDOWS := 1
else
    # Linux and other ELF platforms: .so with SONAME so the dynamic
    # linker can find the right major version at load time.
    SOEXT     := so
    SHLIB     := libjmd.$(SOEXT).$(VERSION)
    SHLINK1   := libjmd.$(SOEXT).$(VERSION_MAJOR)
    SHLINK2   := libjmd.$(SOEXT)
    SHFLAGS   := -shared -Wl,-soname,$(SHLINK1)
    IMPLIB    :=
    PIC_FLAG  := -fPIC
    IS_WINDOWS := 0
endif

CFLAGS_ALL := $(STD) $(WARN) $(OPT) $(PIC_FLAG) -Iinclude \
              -DLIBJMD_BUILDING_DLL $(CFLAGS)

# Note: LIBJMD_BUILDING_DLL is defined unconditionally. On Windows it
# flips LIBJMD_API to __declspec(dllexport) (what we want when compiling
# the library itself). On Linux/macOS the Windows branch in libjmd.h is
# not entered at all, so the flag is silently ignored there.

# Static library: built on Unix-likes. Skipped on Windows because the
# dllexport-decorated objects are not safe to consume from an .a —
# Windows users link the DLL via the import library instead.
ifeq ($(IS_WINDOWS),1)
    STLIB    :=
    # On Windows test_link needs LIBJMD_STATIC undefined (consumer sees
    # dllimport) and links against the import library. The DLL must be
    # discoverable at run time — the Makefile leaves it next to test_link,
    # and the `test` target runs from the build dir where the DLL lives.
    TEST_LIB := $(IMPLIB)
else
    STLIB    := libjmd.a
    # On Unix we link the test against the static lib so the binary is
    # self-contained; no LD_LIBRARY_PATH gymnastics for `make test`.
    # Consumers that use the shared library pick it up via pkg-config.
    TEST_LIB := $(STLIB)
endif

SRCS := src/version.c src/tokenizer.c src/scalars.c src/frontmatter.c \
        src/parser.c src/dom.c src/serializer.c
OBJS := $(SRCS:.c=.o)

PC_FILE := libjmd.pc

.PHONY: all lib test install uninstall clean

all: lib $(PC_FILE)

lib: $(SHLIB) $(STLIB)

$(SHLIB): $(OBJS)
	$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Static archive built only on Unix-likes (see STLIB definition above).
$(STLIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

# The Windows import library is produced as a side effect of linking
# the DLL (via -Wl,--out-implib). We declare it as a phony dependency
# on SHLIB so `make` rebuilds it whenever the DLL is rebuilt.
ifeq ($(IS_WINDOWS),1)
$(IMPLIB): $(SHLIB)
endif

%.o: %.c include/libjmd.h
	$(CC) $(CFLAGS_ALL) -c $< -o $@

$(PC_FILE): libjmd.pc.in Makefile
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' $< > $@

tests/test_link: tests/test_link.c $(TEST_LIB) include/libjmd.h
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_link.c $(TEST_LIB) $(LDFLAGS)

# Unit tests per internal module. Each links against the static lib
# so it can call internal `src/<module>.c` symbols without going via
# the public header. New module tests get a rule + a `test:` dep here.
tests/test_tokenizer: tests/test_tokenizer.c tests/test_util.h \
                      src/tokenizer.h $(TEST_LIB)
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_tokenizer.c $(TEST_LIB) $(LDFLAGS)

tests/test_conformance: tests/test_conformance.c tests/test_util.h \
                        $(TEST_LIB) include/libjmd.h
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_conformance.c $(TEST_LIB) $(LDFLAGS)

tests/test_scalars: tests/test_scalars.c tests/test_util.h \
                    src/scalars.h $(TEST_LIB)
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_scalars.c $(TEST_LIB) $(LDFLAGS)

tests/test_frontmatter: tests/test_frontmatter.c tests/test_util.h \
                        src/frontmatter.h src/tokenizer.h $(TEST_LIB)
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_frontmatter.c $(TEST_LIB) $(LDFLAGS)

tests/test_parser: tests/test_parser.c tests/test_util.h \
                   include/libjmd.h $(TEST_LIB)
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_parser.c $(TEST_LIB) $(LDFLAGS)

UNIT_TESTS := tests/test_tokenizer tests/test_scalars \
              tests/test_frontmatter tests/test_parser \
              tests/test_conformance

test: tests/test_link $(UNIT_TESTS)
	./tests/test_link
	@set -e; for t in $(UNIT_TESTS); do echo "==> $$t"; ./$$t; done

install: all
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 include/libjmd.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 $(SHLIB)          $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(PC_FILE)        $(DESTDIR)$(PREFIX)/lib/pkgconfig/
ifeq ($(IS_WINDOWS),1)
	# Windows: install the import library alongside the DLL so
	# consumers can link against -ljmd via pkg-config.
	install -m 644 $(IMPLIB)         $(DESTDIR)$(PREFIX)/lib/
else
	# Unix: install the static library and the SONAME/symlink chain.
	install -m 644 $(STLIB)          $(DESTDIR)$(PREFIX)/lib/
	ln -sf $(SHLIB)   $(DESTDIR)$(PREFIX)/lib/$(SHLINK1)
	ln -sf $(SHLINK1) $(DESTDIR)$(PREFIX)/lib/$(SHLINK2)
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/include/libjmd.h
	rm -f $(DESTDIR)$(PREFIX)/lib/$(SHLIB)
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/$(PC_FILE)
ifeq ($(IS_WINDOWS),1)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(IMPLIB)
else
	rm -f $(DESTDIR)$(PREFIX)/lib/$(STLIB)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(SHLINK1)
	rm -f $(DESTDIR)$(PREFIX)/lib/$(SHLINK2)
endif

clean:
	rm -f $(OBJS) $(SHLIB) $(STLIB) $(IMPLIB) $(PC_FILE) \
          tests/test_link $(UNIT_TESTS) \
          tests/test_link.exe
