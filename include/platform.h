#ifndef KDB_PLATFORM_H
#define KDB_PLATFORM_H

/* Windows has no fcntl()/struct flock, no fsync(), and mkdir() takes one
 * argument instead of two -- everything else this engine uses (dirent.h,
 * open/close/read/write, rename, unlink) is covered by mingw-w64's own
 * compat shims, so this header only needs to plug those three gaps. */

#ifdef _WIN32

#include <io.h>
#include <direct.h>
#include <windows.h>

#define kdb_mkdir(path) _mkdir(path)
#define kdb_fsync(fd)   _commit(fd)

#else

#include <unistd.h>
#include <sys/stat.h>

#define kdb_mkdir(path) mkdir((path), 0755)
#define kdb_fsync(fd)   fsync(fd)

#endif

#endif
