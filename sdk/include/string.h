/* <string.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 * Implemented in sdk/lib/cvm_libc.c (the mem and str routines DOOM uses). */
#ifndef CVM_LIBC_STRING_H
#define CVM_LIBC_STRING_H

#include <stddef.h>   /* size_t, NULL — provided by the compiler (freestanding) */

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
char  *strtok(char *str, const char *delim);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strerror(int errnum);

#endif /* CVM_LIBC_STRING_H */
