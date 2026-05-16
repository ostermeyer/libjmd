/*
 * test_conformance.c — runner for the canonical JMD conformance corpus.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drives the libjmd parser/serializer against every fixture under
 * `vendor/jmd-spec/conformance/{data,delete,tolerance,must-fail}/`
 * and reports per-fixture pass/fail/skip.
 *
 * Lifecycle across M1 slices: each slice in the bottom-up build of
 * libjmd unlocks more fixtures. Until `jmd_parse` actually returns
 * something other than JMD_ERROR_UNIMPLEMENTED, every fixture lands
 * in the SKIP column with a uniform reason. Once a slice ships, the
 * SUPPORTED_FIXTURES whitelist below grows; tests for unlisted
 * fixtures stay SKIPped (no false-positive failure noise during
 * the build-out). The runner exits non-zero only on real failures
 * — skips do not gate CI.
 *
 * Fixture-root discovery: same three-step probe as jmd-impl and
 * jmd-js (env var, vendor submodule, sibling checkout).
 */

#include "libjmd.h"
#include "test_util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- */
/* Fixture-root probe                                                */
/* ---------------------------------------------------------------- */

/* Try a path; return 1 if it exists and is a directory. */
static int is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Return the first existing fixture root or NULL. Caller does not
 * free — returned pointer is either argv-like-static or a static
 * buffer. */
static const char *fixtures_root(void)
{
    const char *env = getenv("JMD_FIXTURES");
    if (env && is_dir(env)) {
        return env;
    }
    /* Walk relative to the executable's likely working directory.
     * Tests run from the libjmd repo root, so the two candidates
     * mirror jmd-impl's _fixtures_root(): vendor submodule first,
     * sibling checkout as fallback. */
    static const char *candidates[] = {
        "vendor/jmd-spec/conformance",
        "../jmd-spec/conformance",
        NULL,
    };
    for (size_t i = 0; candidates[i] != NULL; i++) {
        if (is_dir(candidates[i])) {
            return candidates[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Per-slice supported-fixture whitelist                             */
/* ---------------------------------------------------------------- */

/* As slices land, append the fixture base-names (without .jmd
 * extension) that the parser is now expected to handle correctly.
 * Anything not listed lands in SKIP, with the reason "feature
 * pending". The list is intentionally per-slice-additive so a
 * regression in an earlier slice can't silently mask itself as a
 * skip.
 *
 * Format: "<mode-dir>/<stem>" where mode-dir is one of
 * "data", "delete", "tolerance", "must-fail".
 *
 * Empty so far. Slice 3 (frontmatter) will add "data/frontmatter".
 * Slice 4 (heading-stack) will add the minimal-object family. Etc.
 */
static const char *SUPPORTED_FIXTURES[] = {
    /* Slice 4a — object-only documents. The runner just confirms
     * jmd_parse returns JMD_OK without erroring; deep-equality
     * against the .json sibling lands once the DOM layer is up
     * (slice M3) so we can materialise the parsed value. */
    "data/minimal-object",
    "data/nested-objects",
    "data/scalars",
    "data/multiline-blockquote",
    "data/frontmatter",
    /* Slice 4b — arrays + items + indented continuation + thematic
     * break + root array. */
    "data/arrays-scalar",
    "data/arrays-object",
    "data/root-array",
    "data/mixed-complete",
    "delete/single-id",
    "delete/bulk-scalar",
    "delete/bulk-composite",
    NULL,
};

static int is_supported(const char *mode_dir, const char *stem)
{
    char key[256];
    int n = snprintf(key, sizeof key, "%s/%s", mode_dir, stem);
    if (n <= 0 || (size_t)n >= sizeof key) {
        return 0;
    }
    for (size_t i = 0; SUPPORTED_FIXTURES[i] != NULL; i++) {
        if (strcmp(SUPPORTED_FIXTURES[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Fixture walk                                                      */
/* ---------------------------------------------------------------- */

static int pass_count = 0;
static int fail_count = 0;
static int skip_count = 0;

/* Read a file into a freshly-malloc'd, NUL-terminated buffer.
 * Returns NULL on error. Caller frees with free(). */
static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* Run a single .jmd fixture through jmd_parse with a no-op visitor.
 * For M1-pre-parser builds this just confirms the file is loadable;
 * once the parser lands the visitor will accumulate state for a real
 * comparison against the .json sibling. */
static void exercise_fixture(const char *mode_dir, const char *stem,
                             const char *jmd_path)
{
    size_t len = 0;
    char *src = slurp(jmd_path, &len);
    if (!src) {
        fprintf(stderr, "  FAIL %s/%s: cannot read %s (%s)\n",
                mode_dir, stem, jmd_path, strerror(errno));
        fail_count++;
        return;
    }

    /* No visitor yet — pass NULLs so the parser is exercised through
     * its actual entry-point even when it returns UNIMPLEMENTED. */
    int rc = jmd_parse(src, len, NULL, NULL);
    free(src);

    if (rc == JMD_OK) {
        pass_count++;
        printf("  PASS %s/%s\n", mode_dir, stem);
        return;
    }

    fprintf(stderr,
            "  FAIL %s/%s: jmd_parse returned %d\n",
            mode_dir, stem, rc);
    fail_count++;
}

/* Filter for directory enumeration: accept entries that end in
 * ".jmd". Returns the stem (caller-owned static slot or freshly-
 * allocated copy) on match, NULL otherwise. */
static int has_jmd_ext(const char *name, char *out_stem, size_t cap)
{
    size_t n = strlen(name);
    if (n <= 4 || strcmp(name + n - 4, ".jmd") != 0) {
        return 0;
    }
    if (n - 4 >= cap) {
        return 0;
    }
    memcpy(out_stem, name, n - 4);
    out_stem[n - 4] = '\0';
    return 1;
}

static void walk_mode(const char *root, const char *mode_dir)
{
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", root, mode_dir);
    if (n <= 0 || (size_t)n >= sizeof path) return;

    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char stem[256];
        if (!has_jmd_ext(ent->d_name, stem, sizeof stem)) continue;

        char jmd_path[1280];
        snprintf(jmd_path, sizeof jmd_path,
                 "%s/%s", path, ent->d_name);

        if (!is_supported(mode_dir, stem)) {
            skip_count++;
            /* Quiet skip — uncomment for verbose diagnosis:
             * printf("  SKIP %s/%s (feature pending)\n",
             *        mode_dir, stem); */
            continue;
        }

        exercise_fixture(mode_dir, stem, jmd_path);
    }
    closedir(d);
}

int main(void)
{
    const char *root = fixtures_root();
    if (!root) {
        fprintf(stderr,
            "conformance: fixture root not found.\n"
            "  Looked for $JMD_FIXTURES, then vendor/jmd-spec/\n"
            "  conformance, then ../jmd-spec/conformance.\n"
            "  Hint: `git submodule update --init` from the libjmd\n"
            "  repo root.\n");
        return 0;  /* Not a failure — environmental skip. */
    }

    printf("conformance: fixtures at %s\n", root);

    walk_mode(root, "data");
    walk_mode(root, "delete");
    walk_mode(root, "tolerance");
    walk_mode(root, "must-fail");

    printf("\nconformance: %d passed, %d failed, %d skipped\n",
           pass_count, fail_count, skip_count);

    return fail_count == 0 ? 0 : 1;
}
