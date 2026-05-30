/* <stdio.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * There is no host filesystem: FILE-based I/O is provided so ports compile,
 * but a port that reads bundled assets should point its reader at the
 * cartridge ROM (cron_rom) rather than fopen(). The string-formatting family
 * (snprintf/vsnprintf/sscanf) is the part that carries real weight and is
 * fully implemented in sdk/lib/cron_sys.c. printf-to-console routes to
 * cron_log. */
#ifndef CVM_LIBC_STDIO_H
#define CVM_LIBC_STDIO_H

#include <stddef.h>   /* size_t, NULL */
#include <stdarg.h>   /* va_list — compiler-provided */

typedef struct _CVM_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define BUFSIZ 1024

/* TOOLCHAIN NOTE: the CronoVM translator lowers varargs (va_start), so the
 * variadic entry points below (printf/fprintf/sprintf/snprintf/sscanf) are
 * defined for real in cron_sys.c and translate fine. The va_list cores and the
 * varargs-free cvm_vsnprintf_buf() remain available too, for code that prefers
 * to format from a prebuilt argument block. */
int   printf(const char *fmt, ...);
int   fprintf(FILE *stream, const char *fmt, ...);
int   sprintf(char *str, const char *fmt, ...);
int   snprintf(char *str, size_t size, const char *fmt, ...);
int   sscanf(const char *str, const char *fmt, ...);
int   fscanf(FILE *stream, const char *fmt, ...);

/* va_list cores — always defined, always translate (va_list is char* here and
 * va_arg lowers to plain pointer loads). */
int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *stream, const char *fmt, va_list ap);
int   vsprintf(char *str, const char *fmt, va_list ap);
int   vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

/* Varargs-free cores. `args` points at an i386 vararg block: each argument in
 * a 4-byte slot (8 bytes for a 64-bit/double value), declaration order,
 * little-endian. A port builds this block to print/scan without C ellipsis. */
int   cvm_vsnprintf_buf(char *str, size_t size, const char *fmt, const void *args);
int   cvm_vsscanf(const char *str, const char *fmt, va_list ap);

int   puts(const char *s);
int   fputs(const char *s, FILE *stream);
int   fputc(int c, FILE *stream);
int   putchar(int c);

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int   fseek(FILE *stream, long offset, int whence);
long  ftell(FILE *stream);
int   fflush(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int   fgetc(FILE *stream);
int   ungetc(int c, FILE *stream);

/* Standard getc/putc — defined as equivalents of fgetc/fputc (C allows them to
 * be macros). getchar/putchar round out the set. */
#define getc(stream)   fgetc(stream)
#define putc(c, stream) fputc((c), (stream))
#define getchar()      fgetc(stdin)
int   feof(FILE *stream);
int   ferror(FILE *stream);
void  clearerr(FILE *stream);

/* Expose the cart's baked --rom blob (cron_rom) as a single read-only file at
 * `path`: a subsequent fopen(path, "rb") returns a ROM-backed handle whose
 * reads come straight from ROM. Lets a cart serve a large baked data archive
 * through the normal stdio file API without a host filesystem. */
void  cron_rom_mount(const char *path);
int   remove(const char *path);
int   rename(const char *oldp, const char *newp);
void  perror(const char *s);

#endif /* CVM_LIBC_STDIO_H */
