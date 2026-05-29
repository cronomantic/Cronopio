/* <errno.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * A single global `errno` lives in cvm_libc.c. The E* values are arbitrary
 * small ints — there is no host OS to define a canonical set; ports only
 * compare against these macros and print strerror() (which returns "error").*/
#ifndef CVM_LIBC_ERRNO_H
#define CVM_LIBC_ERRNO_H

extern int errno;

/* Values follow the canonical Linux/glibc numbering so they stay distinct
 * and match what ported code expects; the host has no OS errno of its own. */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32
#define EDOM    33
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ELOOP   40
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ENOTSUP EOPNOTSUPP

#endif /* CVM_LIBC_ERRNO_H */
