#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "formats/text.h"

#include "golden_atof.inc"

#define CHECK(cond, ...) do { (*checks)++; if (!(cond)) { (*failures)++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 bits64(f64 f) { u64 u; memcpy(&u, &f, 8); return u; }

static void test_numbers(int *failures, int *checks) {
    for (size_t k = 0; k < CVF_ARRAY_LEN(atof_cases); k++) {
        CvfStr s = cvf_str(atof_cases[k].s);
        i32 i = cvf_atoi(s);
        f64 f = cvf_atof(s);
        CHECK(i == atof_cases[k].i, "atoi(%s) = %d, want %d", atof_cases[k].s, i, atof_cases[k].i);
        CHECK(bits64(f) == atof_cases[k].f, "atof(%s) = %.17g (%016llx), want %016llx", atof_cases[k].s, f,
              (unsigned long long)bits64(f), (unsigned long long)atof_cases[k].f);
    }
}

static void test_ini(int *failures, int *checks) {
    const char *src =
        "; comment\r\n"
        "[Info]\r\n"
        "name = \"Alucard\" ; trailing\r\n"
        "Author=Someone\r\n"
        "name = second\r\n"
        "[Files ]\r\n"
        "st = states.st\r\n"
        "[Statedef 200]\r\n"
        "type=S\r\n";
    size_t n = strlen(src);
    char *buf = (char *)malloc(n + 1);
    memcpy(buf, src, n + 1);
    CvfLines L;
    cvf_lines_split(&L, buf, n, false);
    int i = 0;
    CvfIni is;
    char name[64];
    CvfStr sub, v;
    CHECK(cvf_ini_read_section(&L, &i, &is, name, sizeof name, &sub), "info section");
    CHECK(strcmp(name, "info") == 0, "name '%s'", name);
    CHECK(cvf_ini_get(&is, "name", &v) && cvf_str_eq(v, "\"Alucard\""), "name value");
    CHECK(cvf_ini_get(&is, "author", &v) && cvf_str_eq(v, "Someone"), "author value");
    cvf_ini_free(&is);
    CHECK(cvf_ini_read_section(&L, &i, &is, name, sizeof name, &sub), "files section");
    CHECK(strcmp(name, "files ") == 0, "files name (Ikemen keeps the space) '%s'", name);
    cvf_ini_free(&is);
    CHECK(cvf_ini_read_section(&L, &i, &is, name, sizeof name, &sub), "statedef section");
    CHECK(strcmp(name, "statedef ") == 0 && cvf_str_eq(sub, "200"), "statedef '%s' '%.*s'", name, (int)sub.n, sub.p);
    i32 t[2] = {0, 0};
    cvf_ini_free(&is);
    cvf_lines_free(&L);
    (void)t;
}

void test_formats(int *failures, int *checks) {
    test_numbers(failures, checks);
    test_ini(failures, checks);
}
