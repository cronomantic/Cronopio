/* <unistd.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * i_system.c / w_file_posix.c include this. No host OS: every entry is a stub
 * implemented in cron_sys.c (access/unlink fail, getcwd returns an empty
 * string). mkdir is declared in <sys/stat.h>. */
#ifndef CVM_LIBC_UNISTD_H
#define CVM_LIBC_UNISTD_H

#include <sys/types.h>

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

int   access(const char *path, int mode);
int   unlink(const char *path);
int   rmdir(const char *path);
char *getcwd(char *buf, size_t size);

/* POSIX fd-level I/O (open() is in <fcntl.h>). A thin fd table over fopen() in
 * cron_sys.c — fds 0/1/2 are the standard streams, N>=3 map to RAM-FS files. */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);

#endif /* CVM_LIBC_UNISTD_H */
