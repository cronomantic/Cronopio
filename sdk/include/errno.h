/* <errno.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * A single global `errno` lives in cvm_libc.c. The E* values are arbitrary
 * small ints — there is no host OS to define a canonical set; ports only
 * compare against these macros and print strerror() (which returns "error").*/
#ifndef CVM_LIBC_ERRNO_H
#define CVM_LIBC_ERRNO_H

extern int errno;

#define EPERM   1
#define ENOENT  2
#define EIO     5
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ERANGE  34

#endif /* CVM_LIBC_ERRNO_H */
