/* <stdio.h> — Cronopio SDK. The stdio IMPLEMENTATION is picolibc's tinystdio
 * (printf/scanf/FILE/fopen, in picolibc.bc); this header just declares the
 * surface carts use. `FILE` is tinystdio's opaque `struct __file` — carts use
 * it only through the API. The FILE layer sits on the POSIX backend
 * (open/read/write/lseek/close) cron_sys.c provides over the RAM-FS + ROM, with
 * the console (fd 1/2) routed to cron_log. There is no host filesystem; a port
 * reading bundled assets points fopen() at the ROM via cron_rom_mount(). */
#ifndef CVM_LIBC_STDIO_H
#define CVM_LIBC_STDIO_H

#include <stddef.h>   /* size_t, NULL */
#include <stdarg.h>   /* va_list — compiler-provided */

typedef struct __file FILE;

extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

#define EOF (-1)
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define BUFSIZ 1024

int   printf(const char *fmt, ...);
int   fprintf(FILE *stream, const char *fmt, ...);
int   sprintf(char *str, const char *fmt, ...);
int   snprintf(char *str, size_t size, const char *fmt, ...);
int   sscanf(const char *str, const char *fmt, ...);
int   fscanf(FILE *stream, const char *fmt, ...);

int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *stream, const char *fmt, va_list ap);
int   vsprintf(char *str, const char *fmt, va_list ap);
int   vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

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
