/* <stdlib.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 * malloc family is backed by cvm_alloc (the cart heap); exit/abort route to
 * cron_exit. Implemented in sdk/lib/cron_sys.c. */
#ifndef CVM_LIBC_STDLIB_H
#define CVM_LIBC_STDLIB_H

#include <stddef.h>   /* size_t, NULL */

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

int    atoi(const char *s);
long   atol(const char *s);
long   strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
double atof(const char *s);
double strtod(const char *s, char **endptr);

int    abs(int n);
long   labs(long n);

void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));

int    rand(void);
void   srand(unsigned int seed);

char  *getenv(const char *name);

void   exit(int status);
void   abort(void);
void   atexit_unsupported(void);  /* placeholder: carts use I_AtExit, not atexit */

#endif /* CVM_LIBC_STDLIB_H */
