/* nfs5-web: force-included (-include win/posix_compat.h) in every runtime file
 * of the Windows x64 build (llvm-mingw, UCRT).  mingw-w64 already gives
 * pthread (winpthreads), unistd, dirent, clock_gettime and nanosleep; this
 * covers the rest of what the nfsu2-web runtime takes from macOS/POSIX. */
#ifndef NFS5_POSIX_COMPAT_H
#define NFS5_POSIX_COMPAT_H
#ifdef _WIN32
#include <stdint.h>
#include <io.h>
#include <direct.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <pthread.h>

/* POSIX mkdir(path, mode); the mode has no meaning on Windows */
#define mkdir(p, m) _mkdir(p)

/* pread/pwrite: positional I/O without moving the descriptor's offset */
static inline ssize_t nfs5_pread(int fd, void *buf, size_t n, off_t off){
    __int64 save = _lseeki64(fd, 0, SEEK_CUR);
    if (save < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _read(fd, buf, (unsigned)n);
    _lseeki64(fd, save, SEEK_SET);
    return r;
}
static inline ssize_t nfs5_pwrite(int fd, const void *buf, size_t n, off_t off){
    __int64 save = _lseeki64(fd, 0, SEEK_CUR);
    if (save < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _write(fd, buf, (unsigned)n);
    _lseeki64(fd, save, SEEK_SET);
    return r;
}
#define pread  nfs5_pread
#define pwrite nfs5_pwrite

#define fsync(fd) _commit(fd)

/* macOS pthread_setname_np(name) names the calling thread */
#define pthread_setname_np(n) pthread_setname_np(pthread_self(), (n))

/* time: the _r variants and tm_gmtoff (UCRT has localtime_s/gmtime_s, no gmtoff) */
#include <time.h>
static inline struct tm *nfs5_localtime_r(const time_t *t, struct tm *o){ return localtime_s(o, t) ? 0 : o; }
static inline struct tm *nfs5_gmtime_r(const time_t *t, struct tm *o){ return gmtime_s(o, t) ? 0 : o; }
#define localtime_r nfs5_localtime_r
#define gmtime_r    nfs5_gmtime_r
/* seconds east of UTC for a broken-down LOCAL time (its own DST flag) */
static inline long nfs5_gmtoff(struct tm t){ struct tm a = t, b = t; return (long)(_mkgmtime(&a) - mktime(&b)); }
#define TM_GMTOFF(t) nfs5_gmtoff(t)

/* nanosleep: winpthreads' sleeps in whole 15.6 ms ticks unless some program
 * has raised the system timer resolution (a 1 ms sleep measured 14.9 ms,
 * Sleep(1) 1.9 ms): every runtime wait loop that polls with 1 ms naps ran up
 * to 15x too slow, the frontend took minutes to load.  A high-resolution
 * waitable timer instead (runtime/win/sleep_win.c). */
int nfs5_nanosleep(const struct timespec *req, struct timespec *rem);
#define nanosleep nfs5_nanosleep

/* pause(): park the thread for good */
#define pause() (sleep(0x7FFFFFFFu), 0)

/* files are binary: the runtime reads and writes the game's data byte for byte */
#ifndef O_BINARY
#define O_BINARY 0x8000
#endif
#endif
#endif
