/* <sys/types.h> — Cronopio SDK freestanding libc. See ctype.h for rationale.
 *
 * 32-bit (ILP32) target. All POSIX-ish types are 32-bit so nothing here pulls
 * in 64-bit arithmetic the translator can't lower. */
#ifndef CVM_LIBC_SYS_TYPES_H
#define CVM_LIBC_SYS_TYPES_H

#include <stddef.h>   /* size_t — compiler-provided */

typedef long          off_t;
typedef unsigned int  mode_t;
typedef long          ssize_t;
typedef long          time_t;
typedef int           pid_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned int  dev_t;
typedef unsigned int  ino_t;
typedef unsigned int  nlink_t;

#endif /* CVM_LIBC_SYS_TYPES_H */
