// ============================================================================
// inode_error_test.c - 测试优化后的 inode 分配错误处理
// ============================================================================

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/errno.h"

// 简单的字符串格式化函数
void format_filename(char *str, int num) {
    str[0] = '/';
    str[1] = 't';
    str[2] = 'e';
    str[3] = 's';
    str[4] = 't';
    str[5] = '_';
    str[6] = 'i';
    str[7] = 'n';
    str[8] = 'o';
    str[9] = 'd';
    str[10] = 'e';
    str[11] = '_';
    
    // 简单的数字转换
    if (num == 0) {
        str[12] = '0';
        str[13] = '\0';
    } else {
        int i = 12;
        while (num > 0) {
            str[i++] = '0' + (num % 10);
            num /= 10;
        }
        str[i] = '\0';
    }
}

// 辅助函数：打印错误信息
void
print_error(const char *operation, int expected_errno)
{
    int err = geterrno();
    printf("[ERROR] %s failed: errno=%d (%s)\n", 
           operation, err, strerror(err));
    if(expected_errno != 0) {
        printf("  Expected errno: %d (%s)\n", 
               expected_errno, strerror(expected_errno));
        if(err == expected_errno) {
            printf("  ✓ Error code matches expected value\n");
        } else {
            printf("  ✗ Error code mismatch!\n");
        }
    }
    printf("\n");
}

// 测试 1: 正常文件创建
void
test_normal_file_creation(void)
{
    printf("=== Test 1: Normal File Creation ===\n");
    
    int fd = open("/test_normal_file", O_CREATE | O_WRONLY);
    if(fd >= 0) {
        printf("✓ Successfully created file, fd=%d\n", fd);
        int err = geterrno();
        printf("  errno = %d (expected: 0)\n", err);
        close(fd);
        unlink("/test_normal_file");
    } else {
        print_error("open(\"/test_normal_file\", O_CREATE)", 0);
    }
    
    printf("\n");
}

// 测试 2: 正常目录创建
void
test_normal_directory_creation(void)
{
    printf("=== Test 2: Normal Directory Creation ===\n");
    
    if(mkdir("/test_normal_dir") == 0) {
        printf("✓ Successfully created directory\n");
        int err = geterrno();
        printf("  errno = %d (expected: 0)\n", err);
        unlink("/test_normal_dir");
    } else {
        print_error("mkdir(\"/test_normal_dir\")", 0);
    }
    
    printf("\n");
}

// 测试 3: 文件已存在的情况
void
test_file_exists(void)
{
    printf("=== Test 3: File Already Exists ===\n");
    
    // 先创建一个文件
    int fd1 = open("/test_exists", O_CREATE | O_WRONLY);
    if(fd1 >= 0) {
        close(fd1);
        
        // 再次尝试创建同名文件
        int fd2 = open("/test_exists", O_CREATE | O_WRONLY);
        if(fd2 >= 0) {
            printf("✓ Successfully opened existing file, fd=%d\n", fd2);
            int err = geterrno();
            printf("  errno = %d (expected: 0)\n", err);
            close(fd2);
        } else {
            print_error("open(\"/test_exists\", O_CREATE) on existing file", 0);
        }
        
        unlink("/test_exists");
    }
    
    printf("\n");
}

// 测试 4: 无效路径
void
test_invalid_path(void)
{
    printf("=== Test 4: Invalid Path ===\n");
    
    // 尝试在不存在的目录中创建文件
    int fd = open("/nonexistent_dir/test_file", O_CREATE | O_WRONLY);
    if(fd < 0) {
        print_error("open(\"/nonexistent_dir/test_file\", O_CREATE)", ENOENT);
    } else {
        printf("✗ Unexpectedly succeeded with invalid path\n");
        close(fd);
    }
    
    printf("\n");
}

// 测试 5: 目录创建冲突
void
test_directory_conflict(void)
{
    printf("=== Test 5: Directory Creation Conflict ===\n");
    
    // 先创建一个文件
    int fd = open("/test_dir_conflict", O_CREATE | O_WRONLY);
    if(fd >= 0) {
        close(fd);
        
        // 尝试创建同名目录
        if(mkdir("/test_dir_conflict") < 0) {
            print_error("mkdir(\"/test_dir_conflict\") on existing file", EEXIST);
        } else {
            printf("✗ Unexpectedly succeeded creating dir over file\n");
            unlink("/test_dir_conflict");
        }
        
        unlink("/test_dir_conflict");
    }
    
    printf("\n");
}

// 测试 6: 设备节点创建
void
test_device_creation(void)
{
    printf("=== Test 6: Device Node Creation ===\n");
    
    // 创建设备节点
    if(mknod("/test_device", 1, 1) == 0) {
        printf("✓ Successfully created device node\n");
        int err = geterrno();
        printf("  errno = %d (expected: 0)\n", err);
        unlink("/test_device");
    } else {
        print_error("mknod(\"/test_device\", 1, 1)", 0);
    }
    
    printf("\n");
}

// 测试 7: 大量文件创建（测试 inode 耗尽）
void
test_inode_exhaustion(void)
{
    printf("=== Test 7: Inode Exhaustion Test ===\n");
    printf("Creating many files to test inode allocation...\n");
    
    int success_count = 0;
    int failure_count = 0;
    char filename[64];
    
    for(int i = 0; i < 1000; i++) {
        format_filename(filename, i);
        int fd = open(filename, O_CREATE | O_WRONLY);
        if(fd >= 0) {
            success_count++;
            close(fd);
        } else {
            failure_count++;
            if(failure_count == 1) {
                // 第一次失败时打印错误信息
                int err = geterrno();
                printf("First failure at file %d: errno=%d (%s)\n", 
                       i, err, strerror(err));
                
                if(err == 128 || err == ENOSPC) {  // EFS_INODE_FULL = 128
                    printf("✓ Correctly detected inode exhaustion\n");
                } else {
                    printf("? Unexpected error code for inode exhaustion\n");
                }
            }
            break; // 一旦失败就停止
        }
    }
    
    printf("Created %d files successfully, %d failures\n", 
           success_count, failure_count);
    
    // 清理创建的文件
    for(int i = 0; i < success_count; i++) {
        format_filename(filename, i);
        unlink(filename);
    }
    
    printf("\n");
}

// 测试 8: 错误码持久性
void
test_error_persistence(void)
{
    printf("=== Test 8: Error Code Persistence ===\n");
    
    // 产生一个错误
    int fd = open("/nonexistent/file", O_CREATE | O_WRONLY);
    if(fd < 0) {
        int err1 = geterrno();
        printf("First geterrno() = %d (%s)\n", err1, strerror(err1));
        
        // 再次获取 errno
        int err2 = geterrno();
        printf("Second geterrno() = %d (%s)\n", err2, strerror(err2));
        
        if(err1 == err2) {
            printf("✓ Error code persists across multiple calls\n");
        } else {
            printf("✗ Error code changed unexpectedly!\n");
        }
        
        // 执行成功的操作
        int pid = getpid();
        (void)pid; // 避免未使用警告
        
        int err3 = geterrno();
        printf("After successful operation, errno = %d (expected: 0)\n", err3);
    }
    
    printf("\n");
}

int
main(int argc, char *argv[])
{
    printf("\n");
    printf("========================================\n");
    printf("  Enhanced Inode Error Handling Test   \n");
    printf("========================================\n");
    printf("\n");
    
    test_normal_file_creation();
    test_normal_directory_creation();
    test_file_exists();
    test_invalid_path();
    test_directory_conflict();
    test_device_creation();
    test_inode_exhaustion();
    test_error_persistence();
    
    printf("========================================\n");
    printf("  All Inode Error Tests Completed!    \n");
    printf("========================================\n");
    
    exit(0);
}
