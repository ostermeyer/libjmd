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
    /* Slice 4c — depth-qualified items (§8.6a, §8.6b). */
    "tolerance/depth-plus-one-nested-array",
    "tolerance/depth-plus-one-root-array",
    "tolerance/depth-qualified-disambiguation",
    /* Slice 5 — §7.4 repeated-heading promotion + 3 structured
     * errors (sigil_conflict, repeated_explicit_array,
     * repeated_scalar_key). */
    "tolerance/repeated-headings-promote",
    "tolerance/repeated-headings-three",
    "tolerance/repeated-headings-nested",
    "must-fail/sigil-conflict-bare-then-sigil",
    "must-fail/sigil-conflict-sigil-then-bare",
    "must-fail/repeated-explicit-array",
    "must-fail/repeated-scalar-bare",
    "must-fail/repeated-scalar-heading",
    "must-fail/repeated-scalar-mixed",
    "must-fail/scalar-then-object-heading",
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
 * For now this just confirms jmd_parse returns JMD_OK — deep-equality
 * against the .json sibling lands once the DOM layer ships in M3. */
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

/* ----- must-fail support (§7.4.2 + future structured errors) ----- */

/* Tiny JSON-snippet extractor for the .error.json sidecar files.
 * Looks for a JSON-style `"<key>": "<value>"` pair (or numeric) at
 * top level. Not a full JSON parser — the .error.json files are
 * single-line-ish flat objects per spec convention. Returns 1 if
 * found, 0 otherwise. The slice written to out_buf is NUL-
 * terminated; cap is the buffer's full capacity. */
static int json_extract_string(const char *src,
                               const char *key,
                               char *out_buf, size_t cap)
{
    char needle[64];
    int nl = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (nl <= 0 || (size_t)nl >= sizeof needle) return 0;
    const char *p = strstr(src, needle);
    if (!p) return 0;
    p += nl;
    /* Skip optional whitespace + ':' + whitespace. */
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    return (*p == '"');
}

/* Visitor + ctx for must-fail capture: record the first error's
 * kind into a small fixed buffer. We don't care about subsequent
 * errors — the first one is the structured one we asserted. */
typedef struct {
    char kind[64];
    int  got_error;
} must_fail_capture_t;

static int mf_on_parse_error(void *ctx, const jmd_error_t *err)
{
    must_fail_capture_t *cap = (must_fail_capture_t *)ctx;
    if (cap->got_error) return JMD_OK;  /* Keep the first only. */
    cap->got_error = 1;
    if (err->kind) {
        size_t n = strlen(err->kind);
        if (n >= sizeof cap->kind) n = sizeof cap->kind - 1;
        memcpy(cap->kind, err->kind, n);
        cap->kind[n] = '\0';
    } else {
        cap->kind[0] = '\0';
    }
    return JMD_OK;
}

static const jmd_visitor_t MF_VISITOR = {
    .on_parse_error = mf_on_parse_error,
    /* All other callbacks NULL — must-fail fixtures may emit
     * partial events before the error; we ignore them. */
};

/* Read a must-fail fixture pair: load the .jmd, parse expected
 * kind from the .error.json sibling, run jmd_parse with a capture
 * visitor, verify rc != JMD_OK AND the captured kind matches. */
static void exercise_must_fail(const char *stem,
                               const char *jmd_path,
                               const char *err_path)
{
    size_t len = 0;
    char *src = slurp(jmd_path, &len);
    if (!src) {
        fprintf(stderr, "  FAIL must-fail/%s: cannot read %s\n",
                stem, jmd_path);
        fail_count++;
        return;
    }
    size_t err_len = 0;
    char *err_src = slurp(err_path, &err_len);
    if (!err_src) {
        free(src);
        fprintf(stderr, "  FAIL must-fail/%s: cannot read %s\n",
                stem, err_path);
        fail_count++;
        return;
    }
    char expected_kind[64] = {0};
    if (!json_extract_string(err_src, "kind",
                             expected_kind, sizeof expected_kind)) {
        free(src);
        free(err_src);
        fprintf(stderr,
                "  FAIL must-fail/%s: no 'kind' in %s\n",
                stem, err_path);
        fail_count++;
        return;
    }
    free(err_src);

    must_fail_capture_t cap = {{0}, 0};
    int rc = jmd_parse(src, len, &MF_VISITOR, &cap);
    free(src);

    if (rc == JMD_OK) {
        fprintf(stderr,
                "  FAIL must-fail/%s: parse succeeded (expected error %s)\n",
                stem, expected_kind);
        fail_count++;
        return;
    }
    if (!cap.got_error || strcmp(cap.kind, expected_kind) != 0) {
        fprintf(stderr,
                "  FAIL must-fail/%s: got kind=%s (expected %s)\n",
                stem,
                cap.got_error ? cap.kind : "<no error event>",
                expected_kind);
        fail_count++;
        return;
    }
    pass_count++;
    printf("  PASS must-fail/%s (kind=%s)\n", stem, expected_kind);
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

        if (strcmp(mode_dir, "must-fail") == 0) {
            char err_path[1280];
            snprintf(err_path, sizeof err_path,
                     "%s/%s.error.json", path, stem);
            exercise_must_fail(stem, jmd_path, err_path);
        } else {
            exercise_fixture(mode_dir, stem, jmd_path);
        }
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
