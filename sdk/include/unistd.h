/* <unistd.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * i_system.c / w_file_posix.c include this. No host OS: every entry is a stub
 * implemented in cvm_libc.c (access/unlink fail, getcwd returns an empty
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
char *getcwd(char *buf, size_t size);

#endif /* CVM_LIBC_UNISTD_H */
