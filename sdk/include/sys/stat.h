/* <sys/stat.h> — Cronopio SDK freestanding libc. See ctype.h for rationale.
 *
 * DOOM's m_misc.h includes this for struct stat / stat() / mkdir() / the
 * S_IS* macros. There is no filesystem: stat() always fails and mkdir() is a
 * no-op (implemented in cvm_libc.c). The struct carries only the fields the
 * port reads (st_mode, st_size, st_mtime); all are 32-bit. */
#ifndef CVM_LIBC_SYS_STAT_H
#define CVM_LIBC_SYS_STAT_H

#include <sys/types.h>

struct stat {
    mode_t st_mode;
    off_t  st_size;
    time_t st_mtime;
    time_t st_atime;
    time_t st_ctime;
    dev_t  st_dev;
    ino_t  st_ino;
    nlink_t st_nlink;
};

/* mode bits / file-type masks (octal, classic POSIX values). */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000

#define S_IRWXU  0000700
#define S_IRUSR  0000400
#define S_IWUSR  0000200
#define S_IXUSR  0000100
#define S_IRWXG  0000070
#define S_IRWXO  0000007

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

int stat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);

#endif /* CVM_LIBC_SYS_STAT_H */
