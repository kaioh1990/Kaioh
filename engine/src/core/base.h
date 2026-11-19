// CvF engine: basic types, arenas, strings, logging.
#ifndef CVF_BASE_H
#define CVF_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

#define CVF_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CVF_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CVF_MAX(a, b) ((a) > (b) ? (a) : (b))

// Ikemen's IMax / IErr.
#define CVF_IMAX INT32_MAX
#define CVF_IERR INT32_MIN

// ---------------------------------------------------------------- logging
void cvf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void cvf_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---------------------------------------------------------------- arenas
// Bump allocators. Match state lives in one arena so that saving and
// restoring it for rollback is a plain memory copy; content (sprites,
// compiled scripts) lives in a separate long-lived arena.
typedef struct CvfArena {
    u8 *base;
    size_t used, cap;
    bool owns;
} CvfArena;

void cvf_arena_init(CvfArena *a, size_t cap);
void cvf_arena_free(CvfArena *a);
void *cvf_arena_alloc(CvfArena *a, size_t size, size_t align);
void *cvf_arena_zalloc(CvfArena *a, size_t size, size_t align);
static inline void cvf_arena_reset(CvfArena *a) { a->used = 0; }
#define CVF_NEW(arena, T) ((T *)cvf_arena_zalloc((arena), sizeof(T), _Alignof(T)))
#define CVF_NEW_N(arena, T, n) ((T *)cvf_arena_zalloc((arena), sizeof(T) * (size_t)(n), _Alignof(T)))
char *cvf_arena_strdup(CvfArena *a, const char *s, size_t n);

// ---------------------------------------------------------------- strings
// Non-owning string slice.
typedef struct CvfStr {
    const char *p;
    size_t n;
} CvfStr;

static inline CvfStr cvf_str(const char *s) { return (CvfStr){s, s ? strlen(s) : 0}; }
static inline CvfStr cvf_strn(const char *s, size_t n) { return (CvfStr){s, n}; }
CvfStr cvf_str_trim(CvfStr s);
bool cvf_str_ieq(CvfStr a, const char *b);       // ASCII case-insensitive
bool cvf_str_ieq2(CvfStr a, CvfStr b);
bool cvf_str_eq(CvfStr a, const char *b);
int cvf_str_find(CvfStr s, char c);              // -1 when absent
static inline char cvf_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
void cvf_str_lower_into(CvfStr s, char *out, size_t cap);

// ---------------------------------------------------------------- files
// Reads a whole file; returns NULL when missing. *len receives the size.
// The buffer is NUL-terminated (not counted in len) and must be freed.
u8 *cvf_read_file(const char *path, size_t *len);
// Case-insensitive path resolution (MUGEN content is written for Windows):
// returns a malloc'd path that exists, or NULL.
char *cvf_resolve_path(const char *path);
// Joins a path relative to the directory of base ("chars/a/a.def" + "x.st").
void cvf_path_join_dir(char *out, size_t cap, const char *base, const char *rel);

#endif
