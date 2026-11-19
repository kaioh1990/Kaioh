// Text parsing shared by every MUGEN format: numbers, sections, INI-style
// key/value sections. Ported from Ikemen GO common.go with identical
// semantics (including its quirks), so content parses exactly the same.
#ifndef CVF_TEXT_H
#define CVF_TEXT_H

#include "core/base.h"

i32 cvf_atoi(CvfStr s);            // Atoi
f64 cvf_atof(CvfStr s);            // Atof
f64 cvf_pow10(int n);              // Go math.Pow10 (same tables, same rounding)

// A loaded text file split into lines (BOM removed). Lines are NOT trimmed.
typedef struct CvfLines {
    char *text;     // owned buffer
    CvfStr *lines;  // owned array
    int count;
} CvfLines;

bool cvf_lines_load(CvfLines *out, const char *path);
void cvf_lines_split(CvfLines *out, char *text, size_t len, bool normalize_newlines);
void cvf_lines_free(CvfLines *l);

// SectionName: "[Foo Bar] ; x" -> name "foo " (lowercase, keeps the space,
// like Ikemen) and sub "Bar". Returns false when the line is not a section.
bool cvf_section_name(CvfStr line, char *name, size_t namecap, CvfStr *sub);

// IniSection: key -> value, first occurrence wins, keys lowercase.
typedef struct CvfIniKV {
    char *key;
    CvfStr val;
} CvfIniKV;

typedef struct CvfIni {
    CvfIniKV *kv;
    int count, cap;
} CvfIni;

// ReadIniSection: scans from *i to the next section header, parses it.
// name/sub as cvf_section_name. Returns false when no more sections.
bool cvf_ini_read_section(const CvfLines *L, int *i, CvfIni *is, char *name, size_t namecap, CvfStr *sub);
void cvf_ini_parse(CvfIni *is, const CvfLines *L, int *i);
void cvf_ini_free(CvfIni *is);
bool cvf_ini_get(const CvfIni *is, const char *key, CvfStr *val);
// ReadI32/ReadF32/ReadBool: comma-separated values into out[0..n).
bool cvf_ini_read_i32(const CvfIni *is, const char *key, i32 *out, int n);
bool cvf_ini_read_f32(const CvfIni *is, const char *key, f32 *out, int n);
bool cvf_ini_read_bool(const CvfIni *is, const char *key, bool *out, int n);

#endif
