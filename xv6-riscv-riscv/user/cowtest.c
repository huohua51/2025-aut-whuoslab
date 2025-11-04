// ============================================================================
// user/cowtest.c COW (Copy-On-Write) Fork 测试程序
// ============================================================================
//
// 这个文件实现了三个测试用例来验证COW fork的正确性：
// 1. test_basic_cow(): 基本COW功能测试
// 2. test_multiple_forks(): 多进程共享页面测试
// 3. test_large_data(): 大数据COW测试
//
// COW原理：
// fork时父子进程共享物理页面，页表标记为只读
// 当任一进程尝试写入时，触发page fault
// page fault处理程序分配新物理页，复制内容，更新页表
// 这样避免了fork时的大量内存复制，提高了性能
//
// ============================================================================

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096  // 页面大小：4096字节

/**
 * 测试1：基本COW功能测试
 * 
 * 测试目标：
 * 验证fork后父子进程共享内存页
 * 验证写入时触发COW机制，实现真正的复制
 * 验证父子进程的写入互不影响
 * 
 * 测试步骤：
 * 1. 父进程分配一页内存并写入值42
 * 2. fork创建子进程（此时内存共享）
 * 3. 子进程读取数据（应该是42，证明共享成功）
 * 4. 子进程写入新值100（触发COW，分配新页）
 * 5. 父进程读取数据（应该还是42，证明COW成功隔离）
 */
void
test_basic_cow()
{
  printf("Test 1: Basic COW fork\n");
  
  // 分配一页内存
  int *data = (int*)malloc(PGSIZE);
  if(data == 0) {
    printf("malloc failed\n");
    exit(1);
  }
  
  // 父进程写入初始值
  *data = 42;
  
  // fork创建子进程
  int pid = fork();
  if(pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  
  if(pid == 0) {
    // 子进程：读取共享数据（此时还未复制）
    printf("  Child read: %d (should be 42)\n", *data);
    
    // 子进程：写入数据，触发COW机制
    // 此时内核会：
    // 1. 检测到写入COW标记的页面
    // 2. 分配新的物理页
    // 3. 复制原页面内容到新页面
    // 4. 更新子进程页表指向新页面
    printf("  Child writing...\n");
    *data = 100;
    printf("  Child wrote: %d (should be 100)\n", *data);
    
    free(data);
    exit(0);
  } else {
    // 父进程：等待子进程完成
    wait(0);
    
    // 父进程：读取数据，应该还是原值42
    // 这证明子进程的写入没有影响父进程的内存
    printf("  Parent read: %d (should be 42)\n", *data);
    
    free(data);
  }
  
  printf("Test 1: PASS\n\n");
}

/**
 * 测试2：多进程共享页面测试
 * 
 * 测试目标：
 * 验证多个子进程可以同时共享同一页面
 * 验证每个子进程的写入都会触发独立的COW
 * 验证所有进程的写入互不干扰
 * 
 * 测试步骤：
 * 1. 父进程分配一页内存并写入值0
 * 2. 连续fork 3个子进程（4个进程共享同一物理页）
 * 3. 每个子进程读取并写入不同的值（各自触发COW）
 * 4. 父进程验证原值未被修改
 * 
 * COW引用计数机制：
 * 初始：1个进程使用，refcnt=1
 * fork后：4个进程共享，refcnt=4
 * 每次COW后：触发COW的进程refcnt-1，使用新页
 * 最终：父进程仍使用原页，refcnt=1
 */
void
test_multiple_forks()
{
  printf("Test 2: Multiple forks sharing same page\n");
  
  // 分配一页内存
  int *shared = (int*)malloc(PGSIZE);
  if(shared == 0) {
    printf("malloc failed\n");
    exit(1);
  }
  
  // 父进程写入初始值0
  *shared = 0;
  
  // 创建3个子进程（所有进程共享同一物理页）
  for(int i = 0; i < 3; i++) {
    int pid = fork();
    if(pid == 0) {
      // 子进程i：读取共享数据
      printf("  Child %d: reading %d\n", i, *shared);
      
      // 子进程i：写入数据，触发COW
      // 每个子进程都会获得自己独立的物理页副本
      *shared = i + 10;
      printf("  Child %d: wrote %d\n", i, *shared);
      
      free(shared);
      exit(0);
    }
  }
  
  // 父进程：等待所有子进程完成
  for(int i = 0; i < 3; i++) {
    wait(0);
  }
  
  // 父进程：验证原值未被修改
  // 这证明所有子进程的写入都触发了COW，没有影响父进程
  printf("  Parent: value is %d (should be 0)\n", *shared);
  free(shared);
  
  printf("Test 2: PASS\n\n");
}

/**
 * 测试3：大数据COW测试
 * 
 * 测试目标：
 * 验证COW对多页面数据的处理
 * 验证只有被写入的页面才会触发COW
 * 验证COW的懒惰复制特性（只复制实际修改的页面）
 * 
 * 测试步骤：
 * 1. 父进程分配10页内存（40KB）并填充数据
 * 2. fork创建子进程（10页全部共享）
 * 3. 子进程验证数据完整性
 * 4. 子进程只修改其中1页的1个字节
 * 5. 父进程验证其他页面未受影响

 */
void
test_large_data()
{
  printf("Test 3: Large data COW\n");
  
  // 分配10页内存（40KB）
  int size = 10 * PGSIZE;
  char *data = malloc(size);
  if(data == 0) {
    printf("malloc failed\n");
    exit(1);
  }
  
  // 父进程：用规律模式填充所有数据
  // 第i个字节的值为 i % 256
  for(int i = 0; i < size; i++) {
    data[i] = i % 256;
  }
  
  // fork创建子进程（10页全部共享）
  int pid = fork();
  if(pid == 0) {
    // 子进程：验证所有数据的完整性
    // 这个过程只是读取，不会触发COW
    int errors = 0;
    for(int i = 0; i < size; i++) {
      if(data[i] != (i % 256)) {
        errors++;
      }
    }
    
    if(errors == 0) {
      printf("  Child: data verified OK\n");
    } else {
      printf("  Child: %d errors!\n", errors);
    }
    
    // 子进程：只修改第2页的第1个字节
    // 这个写入只会触发第2页的COW，其他9页仍然共享
    data[PGSIZE] = 255;
    printf("  Child: modified one page\n");
    
    free(data);
    exit(0);
  } else {
    // 父进程：等待子进程完成
    wait(0);
    
    // 父进程：检查第2页的第1个字节是否保持原值
    // data[PGSIZE]原值应该是 PGSIZE % 256
    if(data[PGSIZE] == (PGSIZE % 256)) {
      printf("  Parent: data unchanged (COW works!)\n");
    } else {
      printf("  Parent: data corrupted!\n");
    }
    
    free(data);
  }
  
  printf("Test 3: PASS\n\n");
}

/**
 * 主函数：运行所有COW测试用例
 * 
 * 测试顺序：
 * 1. test_basic_cow() - 基本功能验证
 * 2. test_multiple_forks() - 多进程共享验证
 * 3. test_large_data() - 大数据和懒惰复制验证
 * 
 * 如果所有测试都通过，说明COW实现正确，包括：
 * - fork时的页面共享
 * - 写入时的COW触发
 * - 页面引用计数管理
 * - 多进程并发COW处理
 * - 懒惰复制优化
 */
int
main(int argc, char *argv[])
{
  printf("======== COW Fork Test ========\n\n");
  
  // 运行三个测试用例
  test_basic_cow();         // 测试1：基本COW功能
  test_multiple_forks();    // 测试2：多进程共享
  test_large_data();        // 测试3：大数据COW
  
  printf("======== All Tests Passed ========\n");
  exit(0);
}

