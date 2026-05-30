#include <wchar.h>
/* Compatibility stubs for symbols not exported from bionic libc on Android API 17 */
#include <stddef.h>

/* atof - convert string to double */
__attribute__((weak)) double atof(const char *s) {
    double val = 0.0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') { val = val * 10.0 + (*s - '0'); s++; }
    if (*s == '.') {
        double frac = 0.1;
        s++;
        while (*s >= '0' && *s <= '9') { val += (*s - '0') * frac; frac *= 0.1; s++; }
    }
    /* Handle exponent */
    if (*s == 'e' || *s == 'E') {
        int exp = 0, esign = 1;
        s++;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') { s++; }
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        exp *= esign;
        while (exp > 0) { val *= 10; exp--; }
        while (exp < 0) { val /= 10; exp++; }
    }
    return sign * val;
}

/* atoi - convert string to int */
__attribute__((weak)) int atoi(const char *s) {
    int val = 0, sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
    return sign * val;
}

/* strtod */
__attribute__((weak)) double strtod(const char *s, char **end) {
    double v = atof(s);
    while (*s >= '0' && *s <= '9') s++;
    if (*s == '.') while (*s >= '0' && *s <= '9') s++;
    if (*s == 'e' || *s == 'E') { s++; while (*s >= '0' && *s <= '9') s++; }
    if (end) *end = (char *)s;
    return v;
}

/* strtol */
__attribute__((weak)) long strtol(const char *s, char **end, int base) {
    long val = 0, sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if (base == 16) {
        while (*s) {
            int d = (*s >= '0' && *s <= '9') ? *s - '0' :
                    (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
                    (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
            if (d < 0) break;
            val = val * 16 + d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
    }
    if (end) *end = (char *)s;
    return sign * val;
}

/* rand replacement for bionic API 17 */
static unsigned int _compat_seed = 12345u;
__attribute__((weak)) int rand(void) {
    _compat_seed = _compat_seed * 1103515245u + 12345u;
    return (int)(_compat_seed / 65536u) % 32768;
}
__attribute__((weak)) void srand(unsigned int seed) {
    _compat_seed = seed;
}
#define RAND_MAX 2147483647

/* strtof - convert string to float */
__attribute__((weak)) float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

/* strtold - convert string to long double */
__attribute__((weak)) long double strtold(const char *s, char **end) {
    return (long double)strtod(s, end);
}

/* strtoll */
__attribute__((weak)) long long strtoll(const char *s, char **end, int base) {
    return (long long)strtol(s, end, base);
}

/* strtoul */
__attribute__((weak)) unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base);
}

/* strtoull */
__attribute__((weak)) unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtol(s, end, base);
}

/* Wide character conversion stubs */
__attribute__((weak)) long long wcstoll(const wchar_t *s, wchar_t **end, int base) { return 0; }
__attribute__((weak)) unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base) { return 0; }
__attribute__((weak)) double wcstod(const wchar_t *s, wchar_t **end) { return 0.0; }
__attribute__((weak)) long double wcstold(const wchar_t *s, wchar_t **end) { return 0.0L; }
__attribute__((weak)) wint_t btowc(int c) { return (wint_t)c; }
__attribute__((weak)) wint_t fgetwc(FILE *f) { return fgetc(f); }
__attribute__((weak)) wint_t fputwc(wchar_t c, FILE *f) { return fputc(c, f); }
__attribute__((weak)) wint_t getwc(FILE *f) { return fgetc(f); }

/* Locale stubs */
__attribute__((weak)) void *newlocale(int cat, const char *locale, void *base) { return NULL; }
__attribute__((weak)) void freelocale(void *loc) {}
__attribute__((weak)) void *duplocale(void *loc) { return NULL; }

/* Directory stubs */
__attribute__((weak)) void *opendir(const char *name) { return NULL; }
__attribute__((weak)) void closedir(void *dir) {}
__attribute__((weak)) void *fdopendir(int fd) { return NULL; }

/* File position stubs */

/* Permission stubs */
__attribute__((weak)) int fchmod(int fd, unsigned short mode) { return 0; }
__attribute__((weak)) int fchmodat(int dirfd, const char *path, unsigned short mode, int flag) { return 0; }
__attribute__((weak)) int ftruncate(int fd, long long length) { return 0; }

/* Misc */
__attribute__((weak)) int isatty(int fd) { return 0; }
__attribute__((weak)) int chdir(const char *path) { return 0; }
__attribute__((weak)) char *getcwd(char *buf, size_t size) { return NULL; }
__attribute__((weak)) void closelog(void) {}

/* Wide char locale-aware classification */
__attribute__((weak)) int iswalpha_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswblank_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswcntrl_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswdigit_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswlower_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswprint_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswpunct_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) int iswspace_l(wchar_t c, void *loc) { return 0; }
__attribute__((weak)) float wcstof(const wchar_t *s, wchar_t **end) { return 0.0f; }
__attribute__((weak)) int mblen(const char *s, size_t n) { return 1; }
__attribute__((weak)) int mbtowc(wchar_t *pw, const char *s, size_t n) { return 1; }
__attribute__((weak)) int wctomb(char *s, wchar_t wc) { return 1; }
__attribute__((weak)) size_t mbstowcs(wchar_t *pw, const char *s, size_t n) { return 0; }
__attribute__((weak)) size_t wcstombs(char *s, const wchar_t *pw, size_t n) { return 0; }
__attribute__((weak)) void android_set_abort_message(const char *msg) {}
