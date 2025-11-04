
// user/bench_cow.c - COW vs 传统Fork性能基准测试

//
// 这个文件实现了COW（Copy-on-Write）与传统fork的性能对比测试
// 通过三种不同场景测试两种实现方式的性能差异：
// 1. no-touch: 纯fork，子进程不写内存
// 2. touch-1pages: 轻量写入，子进程只写1页
// 3. touch-128pages: 大量写入，子进程写128页
//



#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/riscv.h" // for PGSIZE


// 计时工具函数


/**
 * 获取当前系统tick数
 * @return 当前tick数（xv6中1tick = 10ms）
 */
static inline uint64 now_ticks(){
  return uptime(); // 10ms per tick
}


// 核心测试函数


/**
 * 执行纯fork测试（no-touch场景）
 * 子进程fork后立即exit，不进行任何内存写入操作
 * 这个场景主要测试COW在"只fork不写"情况下的性能优势
 * 
 * @param ops 要执行的fork操作次数
 * @return 执行这些fork操作所花费的tick数
 */
static uint64 run_forks_no_touch(int ops){
  uint64 t0 = now_ticks(); // 记录开始时间
  
  for(int i=0;i<ops;i++){
    int pid = fork();
    if(pid < 0){ 
      printf("fork failed\n"); 
      exit(1); 
    }
    if(pid == 0){
      // 子进程：立即退出，不写任何内存
      exit(0);
    }
    wait(0); // 父进程等待子进程结束
  }
  
  uint64 t1 = now_ticks(); // 记录结束时间
  return t1 - t0; // 返回总耗时
}

/**
 * 触摸指定数量的页面，触发COW机制
 * 对每个页面进行一个字节的写入操作，确保触发页面的COW复制
 * 
 * @param base 内存区域的起始地址
 * @param pages 要触摸的页面数量
 */
static void touch_pages(char *base, int pages){
  for(int i=0;i<pages;i++){
    // 对每个页面的第一个字节进行异或操作
    // 这确保每个页面都会被写入，触发COW机制
    base[i*PGSIZE] ^= 1;
  }
}

/**
 * 执行带内存写入的fork测试（touch场景）
 * 子进程fork后会写入指定数量的页面，测试COW在写入时的性能表现
 * 
 * @param ops 要执行的fork操作次数
 * @param start 要写入的内存区域起始地址
 * @param pages 每个子进程要写入的页面数量
 * @return 执行这些fork+写入操作所花费的tick数
 */
static uint64 run_forks_touch(int ops, char *start, int pages){
  uint64 t0 = now_ticks(); // 记录开始时间
  
  for(int i=0;i<ops;i++){
    int pid = fork();
    if(pid < 0){ 
      printf("fork failed\n"); 
      exit(1); 
    }
    if(pid == 0){
      // 子进程：写入指定数量的页面，触发COW机制
      touch_pages(start, pages);
      exit(0);
    }
    wait(0); // 父进程等待子进程结束
  }
  
  uint64 t1 = now_ticks(); // 记录结束时间
  return t1 - t0; // 返回总耗时
}


// 内存管理辅助函数


/**
 * 确保有足够的内存区域用于测试
 * 通过sbrk系统调用分配指定数量的页面，并进行预热
 * 
 * @param pages 需要分配的页面数量
 * @return 分配的内存区域起始地址
 */
static char* ensure_region(int pages){
  int bytes = pages * PGSIZE; // 计算需要的字节数
  
  // 使用sbrk分配内存
  if(sbrk(bytes) == (void*)-1){
    printf("sbrk failed\n");
    exit(1);
  }
  
  // 获取新分配区域的起始地址
  char *start = (char*)(uint64)sbrk(0) - bytes;
  
  // 预热：读取每个页面的第一个字节，建立页表映射
  // 这样可以减少测试时的缺页中断干扰
  for(int i=0;i<pages;i++){ 
    volatile char x = start[i*PGSIZE]; 
    (void)x; // 避免编译器优化掉这个读取操作
  }
  
  return start;
}


// 主测试程序


/**
 * 主函数：执行COW vs 传统fork的性能对比测试
 * 
 * 命令行参数：
 * @param argc 参数个数
 * @param argv 参数数组
 *   argv[1]: rounds - 测试轮数
 *   argv[2]: ops_no - no-touch场景每轮fork次数
 *   argv[3]: ops_small - touch-1pages场景每轮fork次数  
 *   argv[4]: ops_big - touch-128pages场景每轮fork次数
 *   argv[5]: pages_small - 轻量写入的页面数
 *   argv[6]: pages_big - 大量写入的页面数
 */
int main(int argc, char *argv[]){
  // 默认测试参数
  int rounds = 3;        // 测试轮数
  int ops_no = 100;      // no-touch场景每轮fork次数（放大微小开销）
  int ops_small = 50;    // touch-1pages场景每轮fork次数
  int ops_big = 10;      // touch-128pages场景每轮fork次数
  int pages_small = 1;   // 轻量写入页面数
  int pages_big = 512;   // 大量写入页面数（2MB = 512 * 4096）

  // 解析命令行参数
  if(argc >= 2) rounds = atoi(argv[1]);
  if(argc >= 3) ops_no = atoi(argv[2]);
  if(argc >= 4) ops_small = atoi(argv[3]);
  if(argc >= 5) ops_big = atoi(argv[4]);
  if(argc >= 6) pages_small = atoi(argv[5]);
  if(argc >= 7) pages_big = atoi(argv[6]);

  // 打印测试配置信息
  printf("bench_cow: rounds=%d ops(no/small/big)=%d/%d/%d pages=%d/%d (PGSIZE=%d)\n",
         rounds, ops_no, ops_small, ops_big, pages_small, pages_big, PGSIZE);

  // 预先分配测试用的内存区域，避免在计时过程中进行内存分配
  char *small_region = ensure_region(pages_small);
  char *big_region = ensure_region(pages_big);

 
  // 执行三种测试场景
 

  // 场景1：no-touch测试 - 纯fork，子进程不写内存
  // 这个场景主要测试COW在"只fork不写"情况下的性能优势
  uint64 total_no = 0;
  for(int r=0;r<rounds;r++) total_no += run_forks_no_touch(ops_no);
  uint64 ops_no_total = (uint64)ops_no * (uint64)rounds;
  printf("[no-touch] rounds=%d ops=%lu total_ticks=%lu\n",
         rounds, ops_no_total, total_no);

  // 场景2：touch-1pages测试 - 轻量写入，子进程只写1页
  // 这个场景测试COW在少量写入时的性能表现
  uint64 total_small = 0;
  for(int r=0;r<rounds;r++) total_small += run_forks_touch(ops_small, small_region, pages_small);
  uint64 ops_small_total = (uint64)ops_small * (uint64)rounds;
  printf("[touch-%dpages] rounds=%d ops=%lu total_ticks=%lu\n",
         pages_small, rounds, ops_small_total, total_small);

  // 场景3：touch-128pages测试 - 大量写入，子进程写128页
  // 这个场景测试COW在大量写入时的性能表现
  uint64 total_big = 0;
  for(int r=0;r<rounds;r++) total_big += run_forks_touch(ops_big, big_region, pages_big);
  uint64 ops_big_total = (uint64)ops_big * (uint64)rounds;
  printf("[touch-%dpages] rounds=%d ops=%lu total_ticks=%lu\n",
         pages_big, rounds, ops_big_total, total_big);

  // 测试完成
  printf("done\n");
  exit(0);
}