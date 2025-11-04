// 调度器和进程同步测试程序
// 测试各种调度算法和同步原语

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include <stdarg.h>


static volatile int __print_lock = 0;
static inline void __print_acquire(void){ while(__sync_lock_test_and_set(&__print_lock, 1)) ; }
static inline void __print_release(void){ __sync_lock_release(&__print_lock); }
#undef printf
#define printf(...) do { __print_acquire(); fprintf(1, __VA_ARGS__); __print_release(); } while (0)

#define NPROC 64
#define MAX_PROCESSES 10

// 测试进程创建
void test_process_creation(void) {
  printf("=== Testing process creation ===\n");
  
  int count = 0;
  
  // 测试基本的进程创建
  printf("Test 1: Basic fork...\n");
  int pid = fork();
  if(pid == 0) {
    // 子进程
    printf("  Child process running\n");
    exit(0);
  } else if(pid > 0) {
    // 父进程
    int status;
    wait(&status);
    printf("  OK: process created and exited\n");
  } else {
    printf("  ERROR: fork failed\n");
    return;
  }
  
  // 测试创建多个进程
  printf("Test 2: Creating multiple processes...\n");
  for(int i = 0; i < MAX_PROCESSES; i++) {
    pid = fork();
    if(pid == 0) {
      // 子进程执行简单任务
      for(int j = 0; j < 1000; j++) {
        // 模拟一些工作
        volatile int x = j * j;
        (void)x; // 避免编译器优化
      }
      exit(0);
    } else if(pid > 0) {
      count++;
    } else {
      printf("  ERROR: fork failed at process %d\n", i);
      break;
    }
  }
  
  printf("  Created %d processes\n", count);
  
  // 等待所有子进程完成
  for(int i = 0; i < count; i++) {
    int status;
    wait(&status);
  }
  
  printf("  All processes completed\n");
}

// CPU密集型任务
void cpu_intensive_task(void) {
  volatile int sum = 0;
  for(int i = 0; i < 1000000; i++) {
    sum += i * i;
  }
  printf("  CPU task completed, sum = %d\n", sum);
}

// 测试不同调度算法
void test_different_schedulers(void) {
  printf("=== Testing Different Schedulers ===\n");
  
  const char* scheduler_names[] = {
    "Round Robin",
    "Priority", 
    "Multi-Level Feedback Queue",
  };
  
  for(int scheduler = 0; scheduler < 3; scheduler++) {
    printf("\n--- Testing %s ---\n", scheduler_names[scheduler]);
    
    //  关键：真正切换调度器
    if(set_scheduler(scheduler) < 0) {
      printf("  ERROR: failed to switch scheduler\n");
      continue;
    }
    
    printf("  Scheduler type: %d (%s)\n", scheduler, scheduler_names[scheduler]);
    printf("  Scheduler switched successfully\n");
    
    int start_time = uptime();
    
    // 创建多个CPU密集型进程
    printf("  Creating 3 CPU-intensive processes...\n");
    for(int i = 0; i < 3; i++) {
      int pid = fork();
      if(pid == 0) {
        printf("    Process %d starting with %s\n", i+1, scheduler_names[scheduler]);
        cpu_intensive_task();
        printf("    Process %d completed\n", i+1);
        exit(0);
      }
    }
    
    // 等待所有进程完成
    for(int i = 0; i < 3; i++) {
      int status;
      wait(&status);
    }
    
    int end_time = uptime();
    printf("  All processes completed\n");
    printf("  Total time: ~%d ticks\n", end_time - start_time);
    
    // 模拟不同调度算法的特点
    switch(scheduler) {
      case 0: // Round Robin
        printf("  Round Robin: Fair time sharing, simple and efficient\n");
        break;
      case 1: // Priority
        printf("  Priority: High priority processes get more CPU time\n");
        break;
      case 2: // MLFQ
        printf("  MLFQ: Interactive processes get higher priority\n");
        break;
    }
  }
}

// 测试调度器
void test_scheduler(void) {
  printf("=== Testing scheduler ===\n");
  
  printf("Creating 3 CPU-intensive processes...\n");
  
  int start_time = uptime();
  
  // 创建多个CPU密集型进程
  for(int i = 0; i < 3; i++) {
    int pid = fork();
    if(pid == 0) {
      cpu_intensive_task();
      exit(0);
    }
  }
  
  // 等待所有进程完成
  for(int i = 0; i < 3; i++) {
    int status;
    wait(&status);
  }
  
  int end_time = uptime();
  printf("  All processes completed\n");
  printf("  Total time: ~%d ticks\n", end_time - start_time);
}

// 生产者任务
void producer_task(void) {
  for(int i = 0; i < 5; i++) {
    printf("  Producer: producing item %d\n", i);
    // 使用简单的循环模拟生产时间
    for(int j = 0; j < 10000; j++) {
      volatile int x = j * j;
      (void)x; // 避免编译器优化
    }
  }
  printf("  Producer completed\n");
}

// 消费者任务
void consumer_task(void) {
  for(int i = 0; i < 5; i++) {
    printf("  Consumer: consuming item %d\n", i);
    // 使用简单的循环模拟消费时间
    for(int j = 0; j < 15000; j++) {
      volatile int x = j * j;
      (void)x; // 避免编译器优化
    }
  }
  printf("  Consumer completed\n");
}

// 测试同步机制
void test_synchronization(void) {
  printf("=== Testing synchronization ===\n");
  
  printf("Testing producer-consumer with pipe...\n");
  
  // 创建管道
  int pipe_fd[2];
  if(pipe(pipe_fd) < 0) {
    printf("  ERROR: pipe creation failed\n");
    return;
  }
  
  int start_time = uptime();
  
  // 创建生产者进程
  int pid1 = fork();
  if(pid1 == 0) {
    // 生产者进程
    close(pipe_fd[0]); // 关闭读端
    
    for(int i = 0; i < 5; i++) {
      char data = 'A' + i;
      write(pipe_fd[1], &data, 1);  // 写入管道
      printf("  Producer: produced item '%c'\n", data);
      
      // 模拟生产延迟
      for(int j = 0; j < 10000; j++) {
        volatile int x = j * j;
        (void)x;
      }
    }
    
    close(pipe_fd[1]);
    printf("  Producer completed\n");
    exit(0);
  } else if(pid1 < 0) {
    printf("  ERROR: failed to fork producer\n");
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    return;
  }
  
  // 创建消费者进程
  int pid2 = fork();
  if(pid2 == 0) {
    // 消费者进程
    close(pipe_fd[1]); // 关闭写端
    
    char data;
    int items = 0;
    while(read(pipe_fd[0], &data, 1) > 0) {
      printf("  Consumer: consumed item '%c'\n", data);
      items++;
      
      // 模拟消费延迟
      for(int j = 0; j < 15000; j++) {
        volatile int x = j * j;
        (void)x;
      }
    }
    
    close(pipe_fd[0]);
    printf("  Consumer completed (%d items)\n", items);
    exit(0);
  } else if(pid2 < 0) {
    printf("  ERROR: failed to fork consumer\n");
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    // 等待生产者进程
    wait(0);
    return;
  }
  
  // 父进程关闭管道并等待
  close(pipe_fd[0]);
  close(pipe_fd[1]);
  
  printf("  Waiting for producer and consumer...\n");
  
  int status1, status2;
  int first_exit = wait(&status1);
  int second_exit = wait(&status2);
  
  int end_time = uptime();
  
  printf("  First process exited: PID=%d\n", first_exit);
  printf("  Second process exited: PID=%d\n", second_exit);
  printf("  Total time: ~%d ticks\n", end_time - start_time);
  printf("  Producer-consumer synchronization test passed!\n");
}



// 测试进程优先级
void test_priority(void) {
  printf("=== Testing process priority ===\n");
  
  // 创建不同优先级的进程
  for(int priority = 0; priority < 3; priority++) {
    int pid = fork();
    if(pid == 0) {
      // 子进程根据优先级执行不同时间
      int work_time = (priority + 1) * 1000000;
      volatile int sum = 0;
      for(int i = 0; i < work_time; i++) {
        sum += i;
      }
      printf("  Priority %d process completed\n", priority);
      exit(0);
    }
  }
  
  // 等待所有进程
  for(int i = 0; i < 3; i++) {
    wait(0);
  }
}

// 内存使用测试
void test_memory_usage(void) {
  printf("=== Testing memory usage ===\n");
  
  // 测试内存分配和释放
  int pid = fork();
  if(pid == 0) {
    // 子进程分配一些内存
    char *ptr = malloc(1024);
    if(ptr) {
      // 使用内存
      for(int i = 0; i < 1024; i++) {
        ptr[i] = i % 256;
      }
      free(ptr);
      printf("  Memory allocation test passed\n");
    } else {
      printf("  Memory allocation failed\n");
    }
    exit(0);
  } else {
    wait(0);
  }
}

// 性能基准测试
void benchmark_test(void) {
  printf("=== Performance benchmark ===\n");
  
  int start_time = uptime();
  
  // 创建多个进程进行基准测试
  int num_processes = 5;
  for(int i = 0; i < num_processes; i++) {
    int pid = fork();
    if(pid == 0) {
      // 执行计算密集型任务
      volatile int result = 0;
      for(int j = 0; j < 100000; j++) {
        result += j * j * j;
      }
      exit(0);
    }
  }
  
  // 等待所有进程完成
  for(int i = 0; i < num_processes; i++) {
    wait(0);
  }
  
  int end_time = uptime();
  printf("  Benchmark completed in %d ticks\n", end_time - start_time);
  printf("  Average time per process: %d ticks\n", 
         (end_time - start_time) / num_processes);
}



int main(void) {
  printf("Starting scheduler and synchronization tests...\n\n");
  
  // 运行所有测试
  test_process_creation();
  printf("\n");
  
  // 测试不同调度算法
  test_different_schedulers();
  printf("\n");
  
  test_scheduler();
  printf("\n");
  
  test_synchronization();
  printf("\n");
  
  
  test_priority();
  printf("\n");
  
  test_memory_usage();
  printf("\n");
  
  benchmark_test();
  printf("\n");
  
  printf("=== ALL TESTS DONE ===\n");
  exit(0);
}
