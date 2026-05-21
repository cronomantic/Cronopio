/* <ctype.h> — Cronopio SDK freestanding libc.
 *
 * Carts run on CronoVM with no hosted libc. The SDK ships a minimal C library
 * (headers here, implementation in sdk/lib/cvm_libc.c) so ports written for a
 * hosted environment — e.g. DOOM — compile unchanged. ASCII-only; locale is
 * the C locale. Implemented in cvm_libc.c. */
#ifndef CVM_LIBC_CTYPE_H
#define CVM_LIBC_CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

#endif /* CVM_LIBC_CTYPE_H */
