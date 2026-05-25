/* <fcntl.h> — Cronopio SDK freestanding libc. POSIX file-control: open() and
 * its flags, layered (in cvm_libc.c) over the RAM filesystem that backs fopen.
 * Surfaced by ports that use low-level fd I/O (e.g. Quake's console log). */
#ifndef CVM_LIBC_FCNTL_H
#define CVM_LIBC_FCNTL_H

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

/* mode_t arg (when O_CREAT) is variadic and ignored — the RAM-FS has no perms. */
int open(const char *path, int flags, ...);

#endif /* CVM_LIBC_FCNTL_H */
