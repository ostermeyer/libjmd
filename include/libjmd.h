/*
 * libjmd.h — public API of libjmd, the JMD (JSON Markdown) parser and
 * serializer.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two API levels share a single internal parser:
 *
 *   - Visitor / Event API (jmd_parse, jmd_parse_ex): streaming, zero
 *     allocation in the hot path. Pointers handed to callbacks are
 *     valid only during the callback invocation. Consumers copy what
 *     they want to keep. Best for pipelines and database extensions.
 *
 *   - DOM API (jmd_parse_dom, jmd_parse_dom_ex): builds an owned tree
 *     of jmd_value_t nodes. The envelope owns all its strings; a
 *     single jmd_envelope_free releases everything.
 *
 * Both entry points accept an optional allocator hook so the host
 * (e.g. Postgres with palloc/pfree) can take over internal memory.
 *
 * The JMD format itself is specified in the jmd-spec repository.
 */

#ifndef LIBJMD_H
#define LIBJMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LIBJMD_API — symbol visibility and Windows DLL import/export macro.
 *
 * On Linux and macOS this expands to a GCC/Clang visibility attribute
 * so the library can be compiled with -fvisibility=hidden and only
 * the marked symbols end up in the public ABI. Without the attribute
 * the macro expands to nothing; older compilers simply export
 * everything, which is fine but noisy.
 *
 * On Windows (including MinGW and Cygwin) the macro carries the
 * import/export decoration that the Microsoft ABI demands:
 *
 *   - when building libjmd itself, LIBJMD_BUILDING_DLL is defined by
 *     the Makefile and LIBJMD_API expands to __declspec(dllexport);
 *   - when linking libjmd statically, LIBJMD_STATIC is defined by
 *     the consumer (or by our static build) and LIBJMD_API expands
 *     to nothing;
 *   - otherwise (the consumer links against libjmd.dll) the macro
 *     expands to __declspec(dllimport).
 *
 * MSVC consumers pick the right branch automatically without us
 * having to ship an MSVC-specific header variant. That is the whole
 * point of this macro: one header serves every toolchain.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(LIBJMD_BUILDING_DLL)
#    define LIBJMD_API __declspec(dllexport)
#  elif defined(LIBJMD_STATIC)
#    define LIBJMD_API
#  else
#    define LIBJMD_API __declspec(dllimport)
#  endif
#else
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define LIBJMD_API __attribute__((visibility("default")))
#  else
#    define LIBJMD_API
#  endif
#endif

/* ---------- Version ---------- */

#define LIBJMD_VERSION_MAJOR  0
#define LIBJMD_VERSION_MINOR  1
#define LIBJMD_VERSION_PATCH  0
#define LIBJMD_VERSION_STRING "0.1.0"

/*
 * jmd_version — return the runtime version string of the linked library.
 *
 * Useful for ABI sanity checks. A caller can compare the returned
 * string against the compile-time macro LIBJMD_VERSION_STRING to
 * detect a mismatch between the header it compiled against and the
 * library it actually loaded (e.g. after a package upgrade that left
 * a stale .so on LD_LIBRARY_PATH).
 *
 * Returns: pointer to a static, NUL-terminated string in the form
 * "MAJOR.MINOR.PATCH". The pointer remains valid for the lifetime
 * of the process. The caller does not free it.
 */
LIBJMD_API const char *jmd_version(void);

/* ---------- Return codes ---------- */

#define JMD_OK                   0
#define JMD_ABORT                1   /* visitor aborted cleanly, not an error */
#define JMD_ERROR_PARSE         -1
#define JMD_ERROR_MEMORY        -2
#define JMD_ERROR_INTERNAL      -3
#define JMD_ERROR_UNIMPLEMENTED -99  /* placeholder until feature lands */

/* ---------- Document modes ---------- */

typedef enum {
    JMD_MODE_DATA   = 0,   /*  #   */
    JMD_MODE_QUERY  = 1,   /*  #?  */
    JMD_MODE_SCHEMA = 2,   /*  #!  */
    JMD_MODE_DELETE = 3    /*  #-  */
} jmd_mode_t;

/* ---------- Scalar values ---------- */

typedef enum {
    JMD_SCALAR_NULL   = 0,
    JMD_SCALAR_BOOL   = 1,
    JMD_SCALAR_INT    = 2,
    JMD_SCALAR_FLOAT  = 3,
    JMD_SCALAR_STRING = 4
} jmd_scalar_type_t;

typedef struct {
    jmd_scalar_type_t type;
    union {
        int     boolean;
        int64_t integer;
        double  floating;
        struct {
            const char *ptr;
            size_t      len;
        } string;
    } as;
} jmd_scalar_t;

/* ---------- Errors ---------- */

typedef struct {
    int         line;      /* 1-based */
    int         column;    /* 1-based */
    const char *message;   /* NUL-terminated; borrowed for callback duration */
} jmd_error_t;

/* ---------- Allocator hook ---------- */

/* All function pointers receive the allocator's `ctx` field as first
 * argument so callers can thread a memory context (e.g. Postgres
 * MemoryContext) without globals. Pass NULL to any API to get the
 * libc default (malloc / realloc / free).
 */
typedef struct {
    void *(*alloc)  (void *ctx, size_t n);
    void *(*realloc)(void *ctx, void *p, size_t n);
    void  (*free)   (void *ctx, void *p);
    void  *ctx;
} jmd_allocator_t;

/* ---------- Visitor callbacks ---------- */

/* String parameters (key, label, content) are delivered as (ptr, len)
 * and are valid ONLY during the callback invocation. Consumers that
 * need to keep the bytes MUST copy them before returning.
 *
 * The pointer may refer either to the caller's source buffer (for
 * unmodified runs) or to an internal scratch buffer (for escape-decoded
 * or concatenated content). Either way: valid during the callback,
 * no guarantees afterwards.
 *
 * Return value: JMD_OK continues parsing; any non-zero value aborts
 * parsing and is propagated unchanged as the parse function's return.
 * JMD_ABORT is the conventional "clean stop — not an error".
 *
 * Any callback pointer may be NULL; the corresponding event is then
 * silently ignored. This lets consumers subscribe only to events
 * they care about without writing empty stubs.
 */
typedef struct {
    int (*on_document_start)(void *ctx, jmd_mode_t mode,
                             const char *label, size_t label_len);
    int (*on_document_end)(void *ctx);

    int (*on_frontmatter)(void *ctx,
                          const char *key, size_t key_len,
                          const jmd_scalar_t *value);

    int (*on_object_start)(void *ctx, const char *key, size_t key_len);
    int (*on_object_end)(void *ctx);

    int (*on_array_start)(void *ctx, const char *key, size_t key_len);
    int (*on_array_end)(void *ctx);

    int (*on_item_start)(void *ctx);
    int (*on_item_end)(void *ctx);

    /* on_item_value: emitted INSTEAD of on_item_start / on_item_end
     * for scalar items in an array (e.g. `- 42`, `- hello`). The
     * item is the scalar value itself — no fields follow. Object
     * items use the start/end + field-stream form. */
    int (*on_item_value)(void *ctx, const jmd_scalar_t *value);

    int (*on_field)(void *ctx,
                    const char *key, size_t key_len,
                    const jmd_scalar_t *value);

    int (*on_multiline_field_start)(void *ctx,
                                    const char *key, size_t key_len);
    int (*on_multiline_content)(void *ctx,
                                const char *content, size_t len,
                                int is_paragraph_break);
    int (*on_multiline_field_end)(void *ctx);

    int (*on_scope_reset)(void *ctx);

    int (*on_parse_error)(void *ctx, const jmd_error_t *err);
} jmd_visitor_t;

/* ---------- Parse entry points ---------- */

/*
 * jmd_parse — streaming parse with visitor callbacks.
 *
 * Parses the JMD document at `src` (length `len` bytes) and invokes
 * the visitor's callbacks for every parse event. The visitor's `ctx`
 * is passed through unchanged as the first argument of every callback
 * so consumers can thread their own state.
 *
 * The source buffer must remain alive for the entire duration of the
 * call; libjmd may hand out pointers into it from callbacks. Those
 * pointers are valid only during the callback invocation (see the
 * jmd_visitor_t block comment above for the full rule).
 *
 * Returns:
 *   JMD_OK                 on successful parse.
 *   negative JMD_ERROR_*   on parse failure. If a visitor's
 *                          on_parse_error callback is set it will
 *                          have been invoked before the function
 *                          returns.
 *   any non-zero value a callback returned to abort the parse. This
 *   is propagated unchanged — consumers use it to signal app-specific
 *   conditions (e.g. JMD_ABORT for "found what I was looking for").
 *
 * Thread-safety: safe to call concurrently with itself, provided the
 * visitor and ctx are independent per call.
 */
LIBJMD_API int jmd_parse(const char *src, size_t len,
                         const jmd_visitor_t *visitor, void *ctx);

/*
 * jmd_parse_ex — same as jmd_parse, with an explicit allocator.
 *
 * The allocator governs every internal buffer libjmd may allocate
 * during the parse (scratch buffer for escape decoding, multiline
 * concatenation, etc.). Pass `NULL` to fall back to libc
 * malloc/realloc/free, wrapped in an internal adapter.
 *
 * Typical host use: Postgres passes an allocator whose functions
 * are thin wrappers around palloc/repalloc/pfree. This makes every
 * libjmd allocation live in the current MemoryContext, so a longjmp
 * out of ereport(ERROR, …) automatically reclaims the scratch buffer
 * without libjmd knowing about the longjmp.
 */
LIBJMD_API int jmd_parse_ex(const char *src, size_t len,
                            const jmd_visitor_t *visitor, void *ctx,
                            const jmd_allocator_t *allocator);

/* ---------- DOM API ---------- */

typedef struct jmd_value    jmd_value_t;
typedef struct jmd_envelope jmd_envelope_t;

typedef enum {
    JMD_VAL_NULL   = 0,
    JMD_VAL_BOOL   = 1,
    JMD_VAL_INT    = 2,
    JMD_VAL_FLOAT  = 3,
    JMD_VAL_STRING = 4,
    JMD_VAL_OBJECT = 5,
    JMD_VAL_ARRAY  = 6
} jmd_val_kind_t;

/*
 * jmd_parse_dom — parse into an owned DOM tree.
 *
 * A convenience layer over the visitor API for callers who simply
 * want "the data as a tree". The returned envelope owns every
 * string and every sub-value it transitively contains; the source
 * buffer may be freed or modified after the call returns.
 *
 * Returns: pointer to a heap-allocated envelope, or NULL on error.
 * The caller takes ownership and must release it with
 * jmd_envelope_free. Error detail is not available through this
 * entry point — use jmd_parse_dom_ex if you need line/column.
 */
LIBJMD_API jmd_envelope_t *jmd_parse_dom(const char *src, size_t len);

/*
 * jmd_parse_dom_ex — same as jmd_parse_dom with allocator and
 * optional error output.
 *
 * If `err_out` is non-NULL and parsing fails, it is populated with
 * line/column/message. The message pointer points into static
 * storage in libjmd and remains valid for the process lifetime;
 * the caller must not free it.
 */
LIBJMD_API jmd_envelope_t *jmd_parse_dom_ex(const char *src, size_t len,
                                            const jmd_allocator_t *allocator,
                                            jmd_error_t *err_out);

/*
 * jmd_envelope_free — release an envelope and everything it owns.
 *
 * Passing NULL is a no-op, mirroring free(NULL). If the envelope
 * was allocated via a non-NULL allocator in jmd_parse_dom_ex, the
 * same allocator is used to release memory — libjmd records it in
 * the envelope header so the caller need not re-supply it here.
 */
LIBJMD_API void jmd_envelope_free(jmd_envelope_t *env);

/* Envelope accessors */
LIBJMD_API jmd_mode_t         jmd_envelope_mode  (const jmd_envelope_t *env);
LIBJMD_API const char        *jmd_envelope_label (const jmd_envelope_t *env,
                                                  size_t *out_len);
LIBJMD_API const jmd_value_t *jmd_envelope_value (const jmd_envelope_t *env);

/* Value accessors — behaviour is undefined if the kind doesn't match. */
LIBJMD_API jmd_val_kind_t     jmd_value_kind  (const jmd_value_t *v);
LIBJMD_API int                jmd_value_bool  (const jmd_value_t *v);
LIBJMD_API int64_t            jmd_value_int   (const jmd_value_t *v);
LIBJMD_API double             jmd_value_float (const jmd_value_t *v);
LIBJMD_API const char        *jmd_value_string(const jmd_value_t *v,
                                               size_t *out_len);

/* Container size (object: field count; array: item count). */
LIBJMD_API size_t             jmd_value_size(const jmd_value_t *v);

/* Array indexing. Returns NULL if out of range. */
LIBJMD_API const jmd_value_t *jmd_value_array_item(const jmd_value_t *v,
                                                   size_t i);

/* Object lookup. Returns NULL if not found. */
LIBJMD_API const jmd_value_t *jmd_value_object_get(const jmd_value_t *v,
                                                   const char *key,
                                                   size_t key_len);

/* ---------- Serialization ---------- */

/* Serialize a DOM envelope to JMD text. The output buffer is allocated
 * via `allocator` (or libc if NULL) and stored in *out_buf. The caller
 * frees it with the matching free function.
 */
LIBJMD_API int jmd_serialize_dom(const jmd_envelope_t *env,
                                 char **out_buf, size_t *out_len,
                                 const jmd_allocator_t *allocator);

#ifdef __cplusplus
}
#endif

#endif /* LIBJMD_H */
