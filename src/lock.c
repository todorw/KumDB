#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "../include/lock.h"
#include "../include/error.h"
#include "../include/platform.h"


void kdb_lock_path(const char *table_path, char *out_buf, size_t out_size) {
    snprintf(out_buf, out_size, "%s.lock", table_path);
}


KdbStatus kdb_lock_acquire(KdbLock *lock, const char *table_path, int wait) {
    if (!lock || !table_path) {
        kdb_err_null_arg("lock/table_path", "kdb_lock_acquire");
        return KDB_ERR_BAD_ARG;
    }

    kdb_lock_path(table_path, lock->path, sizeof(lock->path));

    
    int fd = open(lock->path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        kdb_err_io(lock->path, "open lock file");
        return KDB_ERR_IO;
    }

#ifdef _WIN32
    /* LockFileEx over the whole file range is the Windows equivalent of a
     * POSIX whole-file fcntl(F_WRLCK) -- same "one exclusive holder" shape,
     * just a different API. */
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
        close(fd);
        lock->fd = -1;
        if (GetLastError() == ERROR_LOCK_VIOLATION) {
            kdb_err_io_locked(lock->path);
            return KDB_ERR_LOCKED;
        }
        kdb_err_io(lock->path, "LockFileEx");
        return KDB_ERR_IO;
    }
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;

    int cmd = wait ? F_SETLKW : F_SETLK;
    if (fcntl(fd, cmd, &fl) < 0) {
        close(fd);
        lock->fd = -1;
        if (errno == EACCES || errno == EAGAIN) {
            kdb_err_io_locked(lock->path);
            return KDB_ERR_LOCKED;
        }
        kdb_err_io(lock->path, "fcntl lock");
        return KDB_ERR_IO;
    }
#endif

    lock->fd = fd;
    return KDB_OK;
}

void kdb_lock_release(KdbLock *lock) {
    if (!lock || lock->fd < 0) return;

#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(lock->fd);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;

    fcntl(lock->fd, F_SETLK, &fl);
#endif
    close(lock->fd);
    lock->fd = -1;
}

int kdb_lock_is_held(const KdbLock *lock) {
    return lock && lock->fd >= 0;
}


KdbStatus kdb_atomic_write(const char    *dest_path,
                           const uint8_t *data,
                           size_t         len) {
    if (!dest_path || !data) {
        kdb_err_null_arg("dest_path/data", "kdb_atomic_write");
        return KDB_ERR_BAD_ARG;
    }

    
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest_path);

    
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        kdb_err_io(tmp_path, "fopen");
        return KDB_ERR_IO;
    }

    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        unlink(tmp_path);
        kdb_err_io(tmp_path, "fwrite");
        return KDB_ERR_IO;
    }

    
    if (fflush(fp) != 0 || kdb_fsync(fileno(fp)) != 0) {
        fclose(fp);
        unlink(tmp_path);
        kdb_err_io(tmp_path, "fsync");
        return KDB_ERR_IO;
    }
    fclose(fp);

    
    if (rename(tmp_path, dest_path) != 0) {
        unlink(tmp_path);
        kdb_err_io(dest_path, "rename");
        return KDB_ERR_IO;
    }

    /* See kdb__fsync_parent_dir()'s comment in storage.c -- this is the
     * same POSIX rename-durability idiom, deliberately skipped on Windows. */
#ifndef _WIN32
    char dir_path[4096];
    KDB_STRLCPY(dir_path, dest_path, sizeof(dir_path));

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        dir_path[0] = '.'; dir_path[1] = '\0';
    }

    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
#endif

    return KDB_OK;
}