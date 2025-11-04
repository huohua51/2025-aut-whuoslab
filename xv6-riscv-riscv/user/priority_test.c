// ============================================================================
// priority_test.c - 测试优先级调度器
// ============================================================================
//
// 功能：
// 1. 创建多个进程，设置不同的优先级
// 2. 观察调度器是否按优先级调度
// 3. 测试 setpriority 和 getpriority 系统调用
//
// 使用方法：
// 1. 先切换到优先级调度器（在内核中调用 use_priority_scheduler()）
// 2. 运行 priority_test
//
// 预期结果：
// - 高优先级进程先运行
// - 同优先级进程轮转调度
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 工作负载：CPU 密集型计算
void
do_work(int iterations)
{
  int sum = 0;
  for(int i = 0; i < iterations; i++) {
    sum += i;
  }
}

// 测试 1: 基本优先级设置和查询
void
test_basic_priority(void)
{
  printf("=== Test 1: Basic Priority Operations ===\n");
  
  // 获取当前进程的优先级（默认应为 5）
  int priority = getpriority(0);
  printf("Current priority: %d (expected: 5)\n", priority);
  
  // 设置为最高优先级
  if(setpriority(0, 9) == 0) {
    printf("Set priority to 9: SUCCESS\n");
  } else {
    printf("Set priority to 9: FAILED\n");
  }
  
  // 验证是否设置成功
  priority = getpriority(0);
  printf("New priority: %d (expected: 9)\n", priority);
  
  // 测试无效的优先级
  if(setpriority(0, 10) == -1) {
    printf("Reject invalid priority 10: SUCCESS\n");
  } else {
    printf("Reject invalid priority 10: FAILED\n");
  }
  
  // 恢复默认优先级
  setpriority(0, 5);
  printf("\n");
}

// 测试 2: 多进程优先级调度
void
test_priority_scheduling(void)
{
  printf("=== Test 2: Priority Scheduling ===\n");
  printf("Creating 3 processes with different priorities...\n");
  
  for(int i = 0; i < 3; i++) {
    int pid = fork();
    
    if(pid == 0) {
      // 子进程
      int my_priority = 3 + i * 3;  // 优先级：3, 6, 9
      setpriority(0, my_priority);
      
      printf("[PID %d] Priority: %d, Starting work...\n", 
             getpid(), my_priority);
      
      // 执行工作
      uint64 start = uptime();
      for(int j = 0; j < 10; j++) {
        do_work(100000);
        printf("[PID %d] Priority %d: Progress %d/10\n", 
               getpid(), my_priority, j + 1);
      }
      uint64 end = uptime();
      
      printf("[PID %d] Priority %d: Completed in %d ticks\n", 
             getpid(), my_priority, (int)(end - start));
      exit(0);
    }
  }
  
  // 父进程等待所有子进程
  for(int i = 0; i < 3; i++) {
    wait(0);
  }
  
  printf("All child processes completed.\n");
  printf("Note: Higher priority processes should complete first!\n\n");
}

// 测试 3: 动态优先级调整
void
test_dynamic_priority(void)
{
  printf("=== Test 3: Dynamic Priority Adjustment ===\n");
  
  int pid = fork();
  
  if(pid == 0) {
    // 子进程：从低优先级开始
    setpriority(0, 1);
    printf("[Child] Starting with priority 1\n");
    
    for(int i = 0; i < 5; i++) {
      int priority = getpriority(0);
      printf("[Child] Priority %d: Working...\n", priority);
      do_work(50000);
      
      // 逐渐提升优先级
      if(priority < 9) {
        setpriority(0, priority + 2);
        printf("[Child] Increased priority to %d\n", priority + 2);
      }
    }
    
    exit(0);
  } else {
    // 父进程：高优先级
    setpriority(0, 9);
    printf("[Parent] Running with priority 9\n");
    
    for(int i = 0; i < 5; i++) {
      printf("[Parent] Priority 9: Working...\n");
      do_work(50000);
    }
    
    wait(0);
    printf("[Parent] Child completed.\n\n");
  }
}

// 测试 4: 压力测试 - 大量进程
void
test_stress(void)
{
  printf("=== Test 4: Stress Test (10 processes) ===\n");
  
  for(int i = 0; i < 10; i++) {
    int pid = fork();
    
    if(pid == 0) {
      // 子进程：随机优先级
      int priority = (i % 10);  // 优先级 0-9
      setpriority(0, priority);
      
      printf("[PID %d] Priority %d starting\n", getpid(), priority);
      do_work(100000);
      printf("[PID %d] Priority %d finished\n", getpid(), priority);
      
      exit(0);
    }
  }
  
  // 等待所有子进程
  for(int i = 0; i < 10; i++) {
    wait(0);
  }
  
  printf("Stress test completed.\n\n");
}

int
main(int argc, char *argv[])
{
  printf("\n");
  printf("====================================\n");
  printf("  Priority Scheduler Test Suite    \n");
  printf("====================================\n");
  printf("\n");
  
  printf("NOTE: Make sure priority scheduler is enabled!\n");
  printf("      (Call use_priority_scheduler() in kernel)\n\n");
  
  // 运行所有测试
  test_basic_priority();
  test_priority_scheduling();
  test_dynamic_priority();
  test_stress();
  
  printf("====================================\n");
  printf("  All Tests Completed!             \n");
  printf("====================================\n");
  
  exit(0);
}

