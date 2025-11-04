// ============================================================================
// user/errno.h - 用户空间错误码定义
// ============================================================================

#ifndef _USER_ERRNO_H_
#define _USER_ERRNO_H_

// 标准 POSIX 错误码
#define EOK             0
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define ENOTBLK         15
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOLCK          37
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40

// xv6 特定错误
#define EMAXPROC        200
#define EMAXFILE        201
#define EBADPID         202
#define EBADPRIORITY    203

// 错误描述字符串
static const char *error_messages[] = {
    "Success",                          // 0
    "Operation not permitted",          // 1
    "No such file or directory",        // 2
    "No such process",                  // 3
    "Interrupted system call",          // 4
    "I/O error",                        // 5
    "No such device or address",        // 6
    "Argument list too long",           // 7
    "Exec format error",                // 8
    "Bad file descriptor",              // 9
    "No child processes",               // 10
    "Try again",                        // 11
    "Out of memory",                    // 12
    "Permission denied",                // 13
    "Bad address",                      // 14
    "Block device required",            // 15
    "Device or resource busy",          // 16
    "File exists",                      // 17
    "Cross-device link",                // 18
    "No such device",                   // 19
    "Not a directory",                  // 20
    "Is a directory",                   // 21
    "Invalid argument",                 // 22
    "File table overflow",              // 23
    "Too many open files",              // 24
    "Not a typewriter",                 // 25
    "Text file busy",                   // 26
    "File too large",                   // 27
    "No space left on device",          // 28
    "Illegal seek",                     // 29
    "Read-only file system",            // 30
    "Too many links",                   // 31
    "Broken pipe",                      // 32
    "Math argument out of domain",      // 33
    "Math result not representable",    // 34
    "Resource deadlock would occur",    // 35
    "File name too long",               // 36
    "No record locks available",        // 37
    "Function not implemented",         // 38
    "Directory not empty",              // 39
    "Too many symbolic links",          // 40
};

// 获取错误描述
static inline const char*
strerror(int err)
{
    if(err < 0 || err >= 41) {
        return "Unknown error";
    }
    return error_messages[err];
}

#endif // _USER_ERRNO_H_

