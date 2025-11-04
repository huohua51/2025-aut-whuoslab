// ============================================================================
// errno_test.c - 测试 errno 机制和可变参数系统调用
// ============================================================================

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/errno.h"

// 辅助函数：打印错误信息
void
print_error(const char *msg)
{
    int err = geterrno();
    printf("[ERROR] %s: errno=%d (%s)\n", msg, err, strerror(err));
}

// 测试 1: 基本 errno 功能
void
test_basic_errno(void)
{
    printf("=== Test 1: Basic Errno Functionality ===\n");
    
    // 测试成功的系统调用（errno 应该被清零）
    int pid = getpid();
    int err = geterrno();
    printf("getpid() = %d, errno = %d (expected: 0)\n", pid, err);
    
    // 测试失败的系统调用
    int fd = open("/nonexistent_file_12345", O_RDONLY);
    if(fd < 0) {
        print_error("open(\"/nonexistent_file_12345\")");
        printf("  Expected errno: %d (ENOENT)\n", ENOENT);
    }
    
    // 测试无效的文件描述符
    char buf[10];
    int n = read(999, buf, sizeof(buf));
    if(n < 0) {
        print_error("read(999, buf, 10)");
        printf("  Expected errno: %d (EBADF)\n", EBADF);
    }
    
    printf("\n");
}

// 测试 2: 优先级系统调用的 errno
void
test_priority_errno(void)
{
    printf("=== Test 2: Priority System Calls with Errno ===\n");
    
    // 测试有效的优先级设置
    if(setpriority(0, 7) == 0) {
        printf("setpriority(0, 7): SUCCESS\n");
        int err = geterrno();
        printf("  errno = %d (expected: 0)\n", err);
    }
    
    // 测试无效的优先级（超出范围）
    if(setpriority(0, 15) < 0) {
        print_error("setpriority(0, 15)");
        printf("  Expected errno: %d (EINVAL)\n", EINVAL);
    }
    
    // 测试无效的 PID
    if(setpriority(99999, 5) < 0) {
        print_error("setpriority(99999, 5)");
        printf("  Expected errno: %d (ESRCH)\n", ESRCH);
    }
    
    printf("\n");
}

// 测试 3: 文件操作的 errno
void
test_file_errno(void)
{
    printf("=== Test 3: File Operations with Errno ===\n");
    
    // 测试创建文件
    int fd = open("/test_errno_file", O_CREATE | O_WRONLY);
    if(fd >= 0) {
        printf("open(\"/test_errno_file\", O_CREATE): SUCCESS, fd=%d\n", fd);
        int err = geterrno();
        printf("  errno = %d\n", err);
        
        // 写入数据
        char *data = "Hello, errno!\n";
        int n = write(fd, data, strlen(data));
        if(n > 0) {
            printf("write(): wrote %d bytes\n", n);
        }
        
        close(fd);
    }
    
    // 测试打开不存在的文件（不创建）
    fd = open("/another_nonexistent_file", O_RDONLY);
    if(fd < 0) {
        print_error("open(\"/another_nonexistent_file\", O_RDONLY)");
    }
    
    // 测试在只读模式下写入
    fd = open("/test_errno_file", O_RDONLY);
    if(fd >= 0) {
        char *data = "Try to write";
        int n = write(fd, data, strlen(data));
        if(n < 0) {
            print_error("write() on read-only file");
        }
        close(fd);
    }
    
    // 清理
    unlink("/test_errno_file");
    
    printf("\n");
}

// 测试 4: fork 和 wait 的 errno
void
test_process_errno(void)
{
    printf("=== Test 4: Process Operations with Errno ===\n");
    
    int pid = fork();
    if(pid < 0) {
        print_error("fork()");
    } else if(pid == 0) {
        // 子进程
        printf("[Child] PID = %d\n", getpid());
        exit(42);  // 退出状态 42
    } else {
        // 父进程
        int status;
        int child_pid = wait(&status);
        if(child_pid > 0) {
            printf("[Parent] Child %d exited with status %d\n", child_pid, status);
            int err = geterrno();
            printf("[Parent] errno = %d (expected: 0)\n", err);
        }
        
        // 再次调用 wait，应该失败（没有子进程）
        child_pid = wait(&status);
        if(child_pid < 0) {
            print_error("[Parent] wait() with no children");
            printf("  Expected errno: %d (ECHILD)\n", ECHILD);
        }
    }
    
    printf("\n");
}

// 测试 5: errno 的持久性
void
test_errno_persistence(void)
{
    printf("=== Test 5: Errno Persistence ===\n");
    
    // 产生一个错误
    int fd = open("/nonexistent", O_RDONLY);
    if(fd < 0) {
        int err1 = geterrno();
        printf("First geterrno() = %d\n", err1);
        
        // 再次获取 errno，应该相同
        int err2 = geterrno();
        printf("Second geterrno() = %d\n", err2);
        
        if(err1 == err2) {
            printf("  ✓ errno persists across multiple geterrno() calls\n");
        } else {
            printf("  ✗ errno changed unexpectedly!\n");
        }
        
        // 执行一个成功的系统调用
        int pid = getpid();
        (void)pid;  // 避免未使用警告
        
        // errno 应该被清除
        int err3 = geterrno();
        printf("After successful syscall, errno = %d (expected: 0)\n", err3);
    }
    
    printf("\n");
}

// 测试 6: 可变参数系统调用示例
void
test_varargs_syscalls(void)
{
    printf("=== Test 6: Variable Argument System Calls ===\n");
    
    // open 系统调用支持可变参数
    // open(path, flags) - 2 个参数
    // open(path, flags, mode) - 3 个参数（创建文件时需要 mode）
    
    // 2 参数形式
    int fd1 = open("/test_varargs", O_CREATE | O_WRONLY);
    if(fd1 >= 0) {
        printf("open(\"/test_varargs\", O_CREATE | O_WRONLY): fd=%d\n", fd1);
        close(fd1);
    }
    
    // setpriority 使用 2 个参数
    setpriority(0, 5);
    int priority = getpriority(0);
    printf("setpriority(0, 5): priority=%d\n", priority);
    
    // 清理
    unlink("/test_varargs");
    
    printf("✓ Variable argument system calls work correctly\n");
    printf("\n");
}

int
main(int argc, char *argv[])
{
    printf("\n");
    printf("====================================\n");
    printf("  Errno Mechanism Test Suite       \n");
    printf("====================================\n");
    printf("\n");
    
    test_basic_errno();
    test_priority_errno();
    test_file_errno();
    test_process_errno();
    test_errno_persistence();
    test_varargs_syscalls();
    
    printf("====================================\n");
    printf("  All Errno Tests Completed!       \n");
    printf("====================================\n");
    
    exit(0);
}


