
// kernel/sysproc.c - 进程相关系统调用的包装层

//
// 作用：
// 1. 提供用户程序与内核进程管理功能之间的接口
// 2. 从用户空间提取系统调用参数
// 3. 调用内核函数（proc.c 中的实现）
// 4. 返回结果给用户程序
//
// 系统调用流程：
//   用户程序 → ecall 指令 → usertrap() → syscall() → sys_xxx() → k_xxx()
//
// 为什么需要这一层？
// - 隔离：用户不能直接调用内核函数
// - 参数处理：从 trapframe 中提取参数
// - 类型转换：用户地址 vs 内核指针
// - 错误处理：统一的返回值约定
//
// 命名约定：
// - sys_xxx(): 系统调用包装函数（本文件）
// - k_xxx() 或 xxx(): 内核实现函数（proc.c 等）
// - 用户库函数: fork(), exit() 等（user/usys.S）
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "errno.h"

// 外部变量声明
extern struct proc proc[NPROC];


// sys_exit - exit 系统调用的包装函数

//
// 功能：终止当前进程
//
// 用户调用：exit(status)
// - status: 退出状态码（0 表示成功，非 0 表示错误）
//
// 参数传递：
// - 第 0 个参数（a0 寄存器）：退出状态码
// - 通过 argint(0, &n) 提取到 n 变量
//
// 内核函数：kexit(status)
//
// 特殊性：
// - 此函数**永不返回**
// - kexit() 会将进程设为 ZOMBIE 并切换到调度器
// - return 0 这行代码永远不会执行（但编译器要求有返回值）
//
// 使用示例（用户程序）：
//   if(pid == 0) {
//       printf("Child\n");
//       exit(0);  // 子进程正常退出
//   }
//
uint64
sys_exit(void)
{
  int n;                      // 退出状态码
  argint(0, &n);              // 从 a0 寄存器提取参数
  kexit(n);                   // 调用内核的 exit 实现（永不返回）
  return 0;                   // 不会执行到这里
}


// sys_getpid - getpid 系统调用的包装函数

//
// 功能：获取当前进程的 PID
//
// 用户调用：getpid()
// - 无参数
//
// 返回值：当前进程的 PID（正整数）
//
// 实现：
// - 通过 myproc() 获取当前进程指针
// - 返回 p->pid
//
// 使用示例（用户程序）：
//   int pid = getpid();
//   printf("My PID is %d\n", pid);
//
// 线程安全性：
// - myproc() 内部使用 push_off/pop_off 保证安全
// - 读取 pid 是原子操作（单个字段）
//
uint64
sys_getpid(void)
{
  return myproc()->pid;       // 直接返回当前进程的 PID
}


// sys_fork - fork 系统调用的包装函数

//
// 功能：创建子进程（复制当前进程）
//
// 用户调用：fork()
// - 无参数
//
// 返回值：
// - 父进程：返回子进程的 PID（正整数）
// - 子进程：返回 0
// - 失败：返回 -1
//
// 内核函数：kfork()
// - 实现了 COW 优化！
// - 不立即复制物理页面
// - 共享页面，延迟复制
//
// 使用示例（用户程序）：
//   int pid = fork();
//   if(pid == 0) {
//       // 子进程代码
//       printf("I'm child\n");
//       exit(0);
//   } else if(pid > 0) {
//       // 父进程代码
//       printf("Child PID: %d\n", pid);
//       wait(0);
//   } else {
//       // fork 失败
//       printf("fork failed\n");
//   }
//
uint64
sys_fork(void)
{
  return kfork();             // 调用内核的 fork 实现（COW 优化版）
}


// sys_wait - wait 系统调用的包装函数

//
// 功能：等待子进程退出并回收资源
//
// 用户调用：wait(&status) 或 wait(0)
// - status: 用户空间地址，用于接收子进程的退出状态
//   - 如果为 0，不返回退出状态
//   - 如果非 0，将子进程的 xstate 写入此地址
//
// 参数传递：
// - 第 0 个参数（a0 寄存器）：status 指针的地址
// - 通过 argaddr(0, &p) 提取到 p 变量
//
// 返回值：
// - 成功：返回已退出子进程的 PID
// - 失败：返回 -1（没有子进程）
//
// 内核函数：kwait(addr)
//
// 阻塞行为：
// - 如果有活着的子进程，会阻塞等待
// - 如果有僵尸子进程，立即返回
// - 如果没有子进程，立即返回 -1
//
// 使用示例（用户程序）：
//   int pid = fork();
//   if(pid > 0) {
//       int status;
//       int child_pid = wait(&status);
//       printf("Child %d exited with status %d\n", child_pid, status);
//   }
//
uint64
sys_wait(void)
{
  uint64 p;                   // 用户空间地址（指向 int 的指针）
  argaddr(0, &p);             // 从 a0 寄存器提取地址参数
  return kwait(p);            // 调用内核的 wait 实现
}


// sys_sbrk - sbrk 系统调用的包装函数

//
// 功能：增长或收缩进程的堆空间
//
// 用户调用：sbrk(n)
// - n: 要增加/减少的字节数
//   - n > 0: 扩大堆（malloc 使用）
//   - n < 0: 缩小堆（free 使用）
//   - n = 0: 获取当前堆顶地址
//
// 参数：
// - 参数 0 (a0): n - 增长的字节数
// - 参数 1 (a1): t - 分配类型
//   - SBRK_EAGER (0): 立即分配物理内存
//   - SBRK_LAZY (1): 延迟分配（只增加虚拟地址空间）
//
// 返回值：
// - 成功：返回**旧的**堆顶地址
// - 失败：返回 -1
//
// 分配策略：
// 1. **立即分配** (SBRK_EAGER):
//    - 调用 growproc() 立即分配物理页
//    - 优点：访问时不会页错误
//    - 缺点：可能浪费内存
//
// 2. **延迟分配** (SBRK_LAZY):
//    - 只增加 p->sz（虚拟地址空间大小）
//    - 不分配物理页
//    - 首次访问时触发页错误，vmfault() 按需分配
//    - 优点：节省内存（只分配实际使用的页）
//    - 缺点：首次访问会页错误（性能损失）
//
// 使用示例（用户程序）：
//   char *p = sbrk(4096);     // 扩大堆 4KB
//   if(p == (char*)-1) {
//       printf("sbrk failed\n");
//       exit(1);
//   }
//   // 现在可以使用 p[0] 到 p[4095]
//
// malloc 如何使用 sbrk：
//   void* malloc(size_t size) {
//       char *p = sbrk(size);
//       if(p == (char*)-1) return 0;
//       return p;
//   }
//
uint64
sys_sbrk(void)
{
  uint64 addr;                // 旧的堆顶地址（用于返回）
  int t;                      // 分配类型（EAGER 或 LAZY）
  int n;                      // 增长的字节数

  argint(0, &n);              // 提取第 0 个参数：n
  argint(1, &t);              // 提取第 1 个参数：t
  addr = myproc()->sz;        // 保存当前堆顶地址

  if(t == SBRK_EAGER || n < 0) {
    // 情况 1: 立即分配物理内存
    // 情况 2: 缩小内存（必须立即释放）
    if(growproc(n) < 0) {
      return -1;              // 分配失败（内存不足或地址冲突）
    }
  } else {
    // 延迟分配（SBRK_LAZY）
    // 只增加虚拟地址空间，不分配物理页
    
    // 检查整数溢出
    if(addr + n < addr)
      return -1;
    
    // 检查地址空间限制（不能超过 TRAPFRAME）
    if(addr + n > TRAPFRAME)
      return -1;
    
    // 只增加 sz，不调用 uvmalloc
    myproc()->sz += n;        // 更新进程大小
    // 物理页会在首次访问时由 vmfault() 分配
  }
  
  return addr;                // 返回旧的堆顶地址
}


// sys_pause - pause/sleep 系统调用的包装函数

//
// 功能：睡眠指定的时钟滴答数
//
// 用户调用：pause(n) 或 sleep(n)
// - n: 要睡眠的时钟滴答数（tick）
//   - n < 0: 视为 0（不睡眠）
//   - n = 0: 不睡眠，立即返回
//   - n > 0: 睡眠 n 个 tick
//
// 实现原理：
// - 记录当前的 ticks 值（ticks0）
// - 循环检查是否已经过了 n 个 tick
// - 如果未到时间，调用 sleep(&ticks, &tickslock) 睡眠
// - 时钟中断会 wakeup(&ticks)，唤醒所有睡眠在 ticks 上的进程
//
// 提前唤醒：
// - 如果进程被 kill，提前返回 -1
// - 检查 killed(myproc()) 标志
//
// 时间单位：
// - tick: 一次时钟中断的时间间隔
// - 在 xv6 中通常是 10ms（100Hz）
// - 实际时间 = n * 10ms
//
// 使用示例（用户程序）：
//   pause(100);  // 睡眠 100 ticks = 1 秒
//   
//   if(pause(50) == -1) {
//       printf("Interrupted by kill\n");
//   }
//
// 与 Unix sleep 的区别：
// - Unix sleep(seconds): 以秒为单位
// - xv6 pause(ticks): 以 tick 为单位
//
uint64
sys_pause(void)
{
  int n;                      // 睡眠的 tick 数
  uint ticks0;                // 起始时间

  argint(0, &n);              // 提取参数：tick 数
  if(n < 0)
    n = 0;                    // 负数视为 0
    
  acquire(&tickslock);        // 获取 ticks 锁
  ticks0 = ticks;             // 记录起始时间
  
  // 循环等待，直到时间到达
  while(ticks - ticks0 < n){
    // 检查是否被杀死
    if(killed(myproc())){
      release(&tickslock);
      return -1;              // 被杀死，提前返回
    }
    
    // 睡眠在 ticks 通道上
    // 时钟中断会 wakeup(&ticks)，唤醒所有等待的进程
    // 唤醒后重新检查时间是否到达
    sleep(&ticks, &tickslock);
  }
  
  release(&tickslock);        // 释放锁
  return 0;                   // 正常睡眠完成
}


// sys_kill - kill 系统调用的包装函数

//
// 功能：杀死指定 PID 的进程
//
// 用户调用：kill(pid)
// - pid: 目标进程的 PID
//
// 参数传递：
// - 第 0 个参数（a0 寄存器）：目标进程 PID
//
// 返回值：
// - 0: 成功（找到进程并设置 killed 标志）
// - -1: 失败（进程不存在）
//
// 内核函数：kkill(pid)
//
// 工作原理：
// - 不会立即杀死进程
// - 只设置 p->killed = 1
// - 进程在返回用户态时检查此标志并 exit
//
// 为什么不立即杀死？
// - 进程可能持有锁或在关键区
// - 需要给进程机会清理资源
// - 避免内核状态不一致
//
// 使用示例（用户程序）：
//   int pid = fork();
//   if(pid > 0) {
//       sleep(10);
//       kill(pid);  // 杀死子进程
//   }
//
// 与 Unix 的区别：
// - Unix kill 支持信号（SIGTERM, SIGKILL 等）
// - xv6 简化版：只有一种"杀死"操作
//
uint64
sys_kill(void)
{
  int pid;                    // 目标进程 PID

  argint(0, &pid);            // 提取参数：PID
  return kkill(pid);          // 调用内核的 kill 实现
}


// sys_uptime - uptime 系统调用的包装函数
//
// 功能：获取系统启动以来的时钟滴答数
//
// 用户调用：uptime()
// - 无参数
//
// 返回值：系统启动以来的 tick 数（uint64）
//
// 全局变量：ticks
// - 在 trap.c 中定义
// - 每次时钟中断时递增
// - 受 tickslock 保护
//
// 时间计算：
// - 实际运行时间（秒） = uptime() * tick_interval
// - 如果 tick = 10ms，则秒数 = uptime() / 100
//
// 使用示例（用户程序）：
//   uint64 start = uptime();
//   do_work();
//   uint64 end = uptime();
//   printf("Work took %d ticks\n", end - start);
//
// 应用场景：
// - 性能基准测试
// - 超时检测
// - 系统运行时间统计
//
// 线程安全：
// - 使用 tickslock 保护
// - 确保读取的一致性（避免读到中间值）
//
uint64
sys_uptime(void)
{
  uint xticks;                // 临时变量存储 ticks

  acquire(&tickslock);        // 获取锁
  xticks = ticks;             // 读取全局 ticks 变量
  release(&tickslock);        // 释放锁
  
  return xticks;              // 返回时钟滴答数
}


// 系统调用总结

//
// 本文件实现的系统调用（进程相关）：
//
// 1. exit(status)    - 终止进程
// 2. getpid()        - 获取 PID
// 3. fork()          - 创建子进程（COW 优化）
// 4. wait(&status)   - 等待子进程
// 5. sbrk(n)         - 调整堆大小（支持延迟分配）
// 6. pause(n)        - 睡眠 n 个 tick
// 7. kill(pid)       - 杀死进程
// 8. uptime()        - 系统运行时间
//
// 其他系统调用在：
// - sysfile.c: 文件操作（open, read, write, close 等）
// - syscall.c: 系统调用分发框架
//

// 系统调用参数提取函数（在 syscall.c 中定义）

//
// argint(n, *ip)   - 提取第 n 个整数参数
// argaddr(n, *pp)  - 提取第 n 个地址参数
// argstr(n, buf)   - 提取第 n 个字符串参数
//
// 参数位置（RISC-V 调用约定）：
// - 参数 0: a0 寄存器
// - 参数 1: a1 寄存器
// - 参数 2: a2 寄存器
// - 参数 3: a3 寄存器
// - 参数 4: a4 寄存器
// - 参数 5: a5 寄存器
// - 更多参数: 通过栈传递
//
// 返回值：
// - 通过 a0 寄存器返回
// - 系统调用返回后，trapframe->a0 被写回用户的 a0 寄存器


// sys_setpriority - 设置进程优先级

//
// 功能：设置指定进程的调度优先级
//
// 用户调用：setpriority(pid, priority)
// - pid: 目标进程的 PID（0 表示当前进程）
// - priority: 新的优先级值（0-9，数字越大优先级越高）
//
// 返回值：
// - 0: 成功
// - -1: 失败（进程不存在或优先级超出范围）
//
// 优先级范围：
// - 0: 最低优先级
// - 5: 默认优先级
// - 9: 最高优先级
//
// 权限：
// - 任何进程都可以降低自己的优先级
// - 提高优先级需要特权（xv6 简化版暂不限制）
//
// 使用示例（用户程序）：
//   setpriority(0, 9);        // 设置当前进程为最高优先级
//   setpriority(pid, 1);      // 降低子进程优先级
//
uint64
sys_setpriority(void)
{
  int pid;                    // 目标进程 PID
  int priority;               // 新的优先级
  struct proc *p;
  
  argint(0, &pid);            // 提取第 0 个参数：PID
  argint(1, &priority);       // 提取第 1 个参数：优先级
  
  // 验证优先级范围
  if(priority < 0 || priority > 9)
    return -EINVAL;           // 返回负的错误码
  
  // 如果 pid == 0，表示设置当前进程
  if(pid == 0) {
    p = myproc();
    acquire(&p->lock);
    p->priority = priority;
    release(&p->lock);
    return 0;
  }
  
  // 查找指定 PID 的进程
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->pid == pid) {
      p->priority = priority;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  
  return -ESRCH;  // 进程不存在 (No such process)
}


// sys_getpriority - 获取进程优先级

//
// 功能：获取指定进程的调度优先级
//
// 用户调用：getpriority(pid)
// - pid: 目标进程的 PID（0 表示当前进程）
//
// 返回值：
// - 成功：返回优先级值（0-9）
// - 失败：返回 -1（进程不存在）
//
// 使用示例（用户程序）：
//   int priority = getpriority(0);
//   printf("My priority: %d\n", priority);
//
uint64
sys_getpriority(void)
{
  int pid;                    // 目标进程 PID
  int priority;
  struct proc *p;
  
  argint(0, &pid);            // 提取第 0 个参数：PID
  
  // 如果 pid == 0，返回当前进程的优先级
  if(pid == 0) {
    p = myproc();
    acquire(&p->lock);
    priority = p->priority;
    release(&p->lock);
    return priority;
  }
  
  // 查找指定 PID 的进程
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->pid == pid) {
      priority = p->priority;
      release(&p->lock);
      return priority;
    }
    release(&p->lock);
  }
  
  return -ESRCH;  // 进程不存在 (No such process)
}


// sys_geterrno - 获取当前进程的 errno

//
// 功能：获取当前进程最后一次系统调用的错误码
//
// 用户调用：geterrno()
// - 无参数
//
// 返回值：
// - 当前进程的 errno 值
// - 0 表示没有错误
// - 非零表示上次系统调用的错误码
//
// 使用示例（用户程序）：
//   int fd = open("/nonexistent", O_RDONLY);
//   if(fd < 0) {
//       int err = geterrno();
//       printf("open failed with errno = %d\n", err);
//   }
//
// 注意：
// - 每次系统调用都会更新 errno
// - 成功的系统调用会将 errno 设为 0
// - 应该立即检查 errno，否则可能被后续调用覆盖
//
uint64
sys_geterrno(void)
{
  return myproc()->errno;
}

// ============================================================================
// sys_set_scheduler - 设置调度器类型
// ============================================================================
//
// 功能：运行时切换调度算法
//
// 用户调用：set_scheduler(type)
// - type: 调度器类型
//   - 0: Round-Robin（轮转调度，默认）
//   - 1: Priority（优先级调度）
//   - 2: MLFQ（多级反馈队列）
//
// 返回值：
// - 0: 成功
// - -1: 失败（无效的调度器类型）
//
// 使用示例（用户程序）：
//   if(set_scheduler(1) == 0) {
//       printf("Switched to Priority scheduler\n");
//   }
//
// 注意：
// - 切换调度器会立即生效
// - 所有 CPU 核心都会使用新的调度器
// - 进程的调度状态会被保留
//
uint64
sys_set_scheduler(void)
{
  int type;
  argint(0, &type);  // 提取调度器类型参数
  
  // 验证类型是否有效
  if(type < 0 || type > 2) {
    return -EINVAL;  // 无效参数
  }
  
  // 根据类型切换调度器
  switch(type) {
    case 0:
      use_round_robin();           // 轮转调度
      break;
    case 1:
      use_priority_scheduler();    // 优先级调度
      break;
    case 2:
      use_mlfq_scheduler();        // 多级反馈队列
      break;
  }
  
  return 0;  // 成功
}

