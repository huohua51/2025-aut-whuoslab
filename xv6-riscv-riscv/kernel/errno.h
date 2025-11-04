// ============================================================================
// kernel/errno.h - 系统错误码定义
// ============================================================================
//
// 这个文件定义了 xv6 系统的标准错误码
// 兼容 POSIX 标准，可用于内核和用户空间
//

#ifndef _KERNEL_ERRNO_H_
#define _KERNEL_ERRNO_H_

#include <stdint.h>

// ============================================================================
// 标准 POSIX 错误码
// ============================================================================

#define EOK             0   // 成功 (No error)
#define EPERM           1   // 操作不允许 (Operation not permitted)
#define ENOENT          2   // 文件或目录不存在 (No such file or directory)
#define ESRCH           3   // 进程不存在 (No such process)
#define EINTR           4   // 系统调用被中断 (Interrupted system call)
#define EIO             5   // I/O 错误 (I/O error)
#define ENXIO           6   // 设备不存在 (No such device or address)
#define E2BIG           7   // 参数列表过长 (Argument list too long)
#define ENOEXEC         8   // 执行格式错误 (Exec format error)
#define EBADF           9   // 无效的文件描述符 (Bad file descriptor)
#define ECHILD          10  // 没有子进程 (No child processes)
#define EAGAIN          11  // 资源暂时不可用 (Try again)
#define ENOMEM          12  // 内存不足 (Out of memory)
#define EACCES          13  // 权限不足 (Permission denied)
#define EFAULT          14  // 无效的地址 (Bad address)
#define ENOTBLK         15  // 需要块设备 (Block device required)
#define EBUSY           16  // 设备或资源忙 (Device or resource busy)
#define EEXIST          17  // 文件已存在 (File exists)
#define EXDEV           18  // 跨设备链接 (Cross-device link)
#define ENODEV          19  // 设备不存在 (No such device)
#define ENOTDIR         20  // 不是目录 (Not a directory)
#define EISDIR          21  // 是目录 (Is a directory)
#define EINVAL          22  // 无效的参数 (Invalid argument)
#define ENFILE          23  // 系统文件表溢出 (File table overflow)
#define EMFILE          24  // 打开文件过多 (Too many open files)
#define ENOTTY          25  // 不是终端设备 (Not a typewriter)
#define ETXTBSY         26  // 文本文件忙 (Text file busy)
#define EFBIG           27  // 文件过大 (File too large)
#define ENOSPC          28  // 设备空间不足 (No space left on device)
#define ESPIPE          29  // 非法 seek 操作 (Illegal seek)
#define EROFS           30  // 只读文件系统 (Read-only file system)
#define EMLINK          31  // 链接过多 (Too many links)
#define EPIPE           32  // 管道破裂 (Broken pipe)
#define EDOM            33  // 数学参数超出范围 (Math argument out of domain)
#define ERANGE          34  // 数学结果不可表示 (Math result not representable)
#define EDEADLK         35  // 会发生资源死锁 (Resource deadlock would occur)
#define ENAMETOOLONG    36  // 文件名过长 (File name too long)
#define ENOLCK          37  // 没有可用的锁 (No record locks available)
#define ENOSYS          38  // 功能未实现 (Function not implemented)
#define ENOTEMPTY       39  // 目录非空 (Directory not empty)
#define ELOOP           40  // 符号链接层数过多 (Too many symbolic links)

// ============================================================================
// xv6 特定错误码 (从 200 开始)
// ============================================================================

#define EMAXPROC        200 // 进程数量达到上限
#define EMAXFILE        201 // 打开文件数达到上限
#define EBADPID         202 // 无效的进程 ID
#define EBADPRIORITY    203 // 无效的优先级值
#define EBADFD          204 // 无效的文件描述符
#define EBADPATH        205 // 无效的路径
#define EBADADDR        206 // 无效的用户地址
#define EBADARG         207 // 无效的系统调用参数
#define EFS_INODE_FULL  128 // 文件系统inode已满

// ============================================================================
// 辅助宏
// ============================================================================

// 检查是否是错误
#define IS_ERR(x)       ((x) < 0)

// 检查是否成功
#define IS_OK(x)        ((x) >= 0)

// 返回错误码（负数）
#define ERR_PTR(err)    (-(err))

// 从错误指针获取错误码
#define PTR_ERR(ptr)    (-(ptr))

// 错误指针宏（用于返回错误指针）
#define ERR_PTR_CODE(err)    ((void*)(uintptr_t)(err))

// 检查是否是错误指针
#define IS_ERR_PTR(ptr)      ((uintptr_t)(ptr) > (uintptr_t)(-1000))

// 检查是否是有效指针
#define IS_VALID_PTR(ptr)    ((uintptr_t)(ptr) < (uintptr_t)(-1000))

// 从错误指针获取错误码
#define PTR_ERR_CODE(ptr)    ((int)(uintptr_t)(ptr))

// 最大错误号（用于指针验证）
#define MAXERRNO        1000

#endif // _KERNEL_ERRNO_H_

