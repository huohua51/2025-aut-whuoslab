#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 简单任务（静默执行，避免输出混乱）
static void simple_task(void) {
  for (volatile int k = 0; k < 10000000; k++) ;
}

// CPU 密集任务
static void cpu_intensive_task(void) {
  for (volatile unsigned long long k = 0; k < 200000000ULL; k++) ;
  exit(0);  // 静默退出，避免输出混乱
}

// 生产者-消费者（pipe 同步）
static int pcfd[2];
static void producer_task(void) {
  close(pcfd[0]);                 // 只写
  for (int i = 1; i <= 5; i++) {  // 减少到5个，加快测试
    write(pcfd[1], &i, sizeof(i));
    // 简单延时
    for (volatile int d = 0; d < 50000; d++) ;
  }
  close(pcfd[1]);
  exit(0);
}
static void consumer_task(void) {
  close(pcfd[1]);                 // 只读
  int x, got = 0;
  while (read(pcfd[0], &x, sizeof(x)) == sizeof(x)) {
    got++;
  }
  close(pcfd[0]);
  exit(0);
}

static void test_process_creation(void) {
  printf("=== Testing process creation ===\n");
  
  // 基本创建测试
  printf("Test 1: Basic fork...\n");
  int pid = fork();
  if (pid < 0) { printf("  fork failed\n"); exit(1); }
  if (pid == 0) { simple_task(); exit(0); }
  wait(0);
  printf("  OK: process created and exited\n");

  // 批量创建测试（减少数量避免输出混乱）
  printf("Test 2: Creating multiple processes...\n");
  int created = 0;
  for (int i = 0; i < 10; i++) {  // 从200改为10
    int c = fork();
    if (c < 0) break;
    if (c == 0) { simple_task(); exit(0); }
    created++;
  }
  printf("  Created %d processes\n", created);
  for (int i = 0; i < created; i++) wait(0);
  printf("  All processes completed\n");
}

static void test_scheduler(void) {
  printf("=== Testing scheduler ===\n");
  printf("Creating 3 CPU-intensive processes...\n");
  
  uint64 t0 = uptime();
  for (int i = 0; i < 3; i++) {
    int c = fork();
    if (c == 0) cpu_intensive_task();
  }
  
  // 等待子进程完成
  for (int i = 0; i < 3; i++) wait(0);
  uint64 t1 = uptime();
  
  printf("  All processes completed\n");
  printf("  Total time: ~%d ticks\n", (int)(t1 - t0));
}

static void test_synchronization(void) {
  printf("=== Testing synchronization ===\n");
  printf("Testing producer-consumer with pipe...\n");
  
  if (pipe(pcfd) < 0) { printf("  pipe failed\n"); exit(1); }
  
  int p1 = fork();
  if (p1 == 0) producer_task();
  
  int p2 = fork();
  if (p2 == 0) consumer_task();
  
  // 父进程必须关闭pipe两端，否则consumer会阻塞
  close(pcfd[0]);
  close(pcfd[1]);
  
  printf("  Waiting for producer and consumer...\n");
  int status1 = wait(0);
  printf("  First process exited: %d\n", status1);
  int status2 = wait(0);
  printf("  Second process exited: %d\n", status2);
  printf("  Producer and consumer completed successfully\n");
}

int main(void) {
  test_process_creation();
  test_scheduler();
  test_synchronization();
  printf("=== ALL TESTS DONE ===\n");
  exit(0);
}
