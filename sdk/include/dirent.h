/* <dirent.h> — Cronopio SDK freestanding libc. Directory enumeration over the
 * in-RAM filesystem (see cvm_libc.c). The RAM-FS stores files by full path;
 * opendir()/readdir() synthesise directory listings by extracting the
 * immediate child component of every stored path (and the ROM-backed file)
 * under the queried directory. Surfaced by ports that scan directories — e.g.
 * UQM's libs/uio stdio filesystem backend discovering its .uqm content packs.
 *
 * Directories are IMPLICIT: a directory "exists" iff some file lives under it.
 * mkdir() is therefore a no-op success and there are no empty directories. */
#ifndef CVM_LIBC_DIRENT_H
#define CVM_LIBC_DIRENT_H

#include <sys/types.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* d_type values (BSD/Linux subset the ports actually test). */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    ino_t         d_ino;
    unsigned char d_type;
    char          d_name[NAME_MAX + 1];
};

typedef struct __cvm_DIR DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
/* POSIX reentrant variant (UQM's stdio backend uses it). entry is caller
 * storage; *result is set to entry on success, NULL at end-of-directory. */
int            readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
int            closedir(DIR *dirp);
void           rewinddir(DIR *dirp);

#endif /* CVM_LIBC_DIRENT_H */
