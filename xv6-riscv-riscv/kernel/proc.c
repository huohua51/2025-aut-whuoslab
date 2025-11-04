
// kernel/proc.c - 进程管理核心模块

// 主要功能：
// 1. 进程创建和销毁（fork, exit）
// 2. 进程调度（scheduler, sched, yield）
// 3. 进程同步（sleep, wakeup）
// 4. 进程通信和资源管理
//
// 关键数据结构：
// - proc[NPROC]: 进程表（固定大小数组，最多 64 个进程）
// - cpus[NCPU]: CPU 核心状态数组
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"


// 全局变量


struct cpu cpus[NCPU];          // 所有 CPU 核心的状态数组

struct proc proc[NPROC];        // 进程表：固定大小数组
                                // 使用数组而非链表的原因：
                                // 1. 简单可靠，不需要动态内存分配
                                // 2. 遍历效率高（缓存友好）
                                // 3. 索引可以直接映射到内核栈地址

struct proc *initproc;          // 指向 init 进程的指针
                                // init 是第一个用户进程，PID = 1
                                // 所有孤儿进程会被重新指向 init

int nextpid = 1;                // 下一个要分配的 PID
                                // 从 1 开始递增（PID 0 保留）
                                // 受 pid_lock 保护

struct spinlock pid_lock;       // 保护 nextpid 的自旋锁
                                // 多个 CPU 可能同时 fork，需要原子分配 PID

extern void forkret(void);      // fork 子进程第一次调度时的入口函数
static void freeproc(struct proc *p);  // 释放进程资源的内部函数

extern char trampoline[];       // trampoline.S 中定义的跳板代码起始地址

// wait_lock 的作用：
// 1. 保护进程的父子关系（p->parent）
// 2. 确保 wait() 和 exit() 的唤醒不会丢失
// 3. 遵循内存模型，避免重排序导致的问题
// 
// 锁顺序规则（防止死锁）：
// wait_lock 必须在任何 p->lock 之前获取
struct spinlock wait_lock;


// 可插拔调度策略接口（策略模式）
//
// 设计思想：
// - 将"选择下一个进程"的策略与"调度循环框架"分离
// - 通过函数指针实现运行时切换调度算法
// - 避免在 proc.c 中重复调度循环代码
//
// 策略函数规范：
//   struct proc* selector(void)
//   
// 要求：
// - 返回一个 RUNNABLE 状态的进程（已检查状态）
// - 返回 NULL 表示没有可运行进程
// - 不应该修改进程状态（由 scheduler 统一处理）
// - 不应该持有任何锁（避免死锁）
//
// 支持的调度算法：
// - Round-Robin (RR): 简单轮转，公平但可能不够高效
// - Priority: 优先级调度，高优先级先运行
// - MLFQ: 多级反馈队列，兼顾交互性和吞吐量
//

// 前向声明
static struct proc* default_round_robin(void);

// 当前使用的调度策略（函数指针，默认轮转）
// 可通过 set_scheduler() 在运行时切换
// 注意：非 static，允许其他模块（如 trap.c）访问以检查调度器类型
struct proc* (*select_next_proc)(void) = default_round_robin;


// proc_mapstacks - 为所有进程分配并映射内核栈

//
// 调用时机：在 kvminit() 中调用，初始化内核页表时
//
// 内核栈布局（每个进程）：
//   高地址：[Guard Page - 无映射，用于检测栈溢出]
//          [内核栈 - 1 页 (4KB)，可读写]
//   低地址：[下一个进程的 Guard Page...]
//
// 为什么每个进程需要独立的内核栈？
// - 进程在内核态运行时（处理系统调用/中断），需要独立的栈空间
// - 避免进程间的内核栈冲突
// - 支持多核并行处理系统调用
//
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  // 遍历所有进程槽位
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();          // 为内核栈分配一个物理页
    if(pa == 0)
      panic("kalloc");
    
    uint64 va = KSTACK((int) (p - proc));  // 计算此进程的内核栈虚拟地址
                                            // KSTACK 宏会留出 guard page
    
    // 在内核页表中建立映射：虚拟地址 va → 物理地址 pa
    // 权限：PTE_R | PTE_W（可读写，内核专用）
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}


// procinit - 初始化进程表

//
// 调用时机：系统启动时，在 main() 中调用一次
//
// 初始化内容：
// 1. 初始化全局锁（pid_lock, wait_lock）
// 2. 初始化每个进程的锁和状态
// 3. 设置内核栈地址
//
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");      // 初始化 PID 分配锁
  initlock(&wait_lock, "wait_lock");   // 初始化 wait 锁
  
  // 初始化进程表中的每个槽位
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");      // 初始化进程锁
      p->state = UNUSED;               // 标记为未使用
      p->kstack = KSTACK((int) (p - proc));  // 计算内核栈虚拟地址
                                             // 地址在 proc_mapstacks 中已映射
  }
}


// cpuid - 获取当前 CPU 的 ID

//
// 实现：读取 tp 寄存器（Thread Pointer）
// xv6 约定：tp 寄存器存储 CPU 的 hart ID
//
// 为什么必须关中断？
// - 如果开中断，可能在读取 tp 后被切换到另一个 CPU
// - 导致返回错误的 CPU ID
//
// 使用示例：
//   push_off();  // 关中断
//   int id = cpuid();
//   pop_off();   // 恢复中断
//
int
cpuid()
{
  int id = r_tp();    // 读取 tp 寄存器（在 start.c 中设置）
  return id;
}


// mycpu - 获取当前 CPU 的 struct cpu 指针

//
// 前提条件：必须关闭中断（否则可能切换 CPU，返回错误指针）
//
// 返回：指向 cpus[id] 的指针，包含当前 CPU 的状态信息
//
struct cpu*
mycpu(void)
{
  int id = cpuid();           // 获取 CPU ID
  struct cpu *c = &cpus[id];  // 返回对应的 CPU 状态结构体指针
  return c;
}


// myproc - 获取当前进程的 struct proc 指针

//
// 安全设计：
// - 使用 push_off/pop_off 确保在获取指针期间不会被切换 CPU
// - 即使开中断调用也是安全的
//
// 返回：当前在此 CPU 上运行的进程指针，如果没有则返回 0
//
// 使用场景：
// - 系统调用中获取当前进程
// - 中断处理中识别被中断的进程
//
struct proc*
myproc(void)
{
  push_off();                 // 关中断并增加锁计数
  struct cpu *c = mycpu();    // 获取当前 CPU 结构体
  struct proc *p = c->proc;   // 读取当前运行的进程指针
  pop_off();                  // 恢复中断状态
  return p;
}


// allocpid - 分配一个新的进程 ID

//
// 线程安全：使用 pid_lock 保护 nextpid
//
// PID 分配策略：简单递增
// - 优点：实现简单，分配快速
// - 缺点：PID 可能溢出（但对 xv6 不是问题）
//
// 生产级系统的改进：
// - 使用位图标记已用 PID
// - PID 命名空间（容器隔离）
// - 防止 PID 回绕攻击
//
int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);         // 获取锁，保证原子性
  pid = nextpid;              // 读取下一个 PID
  nextpid = nextpid + 1;      // 递增
  release(&pid_lock);         // 释放锁
  
  return pid;
}


// allocproc - 在进程表中查找并分配一个空闲的进程槽

//
// 查找策略：线性扫描进程表
// - 时间复杂度：O(NPROC)
//
// 返回值：
// - 成功：返回进程指针，并持有 p->lock
// - 失败：返回 0（进程表已满或内存分配失败）
//
// 初始化内容：
// 1. 分配 PID
// 2. 分配 trapframe 页
// 3. 创建用户页表
// 4. 设置进程上下文（ra 指向 forkret）
//
// 调用者责任：
// - 调用者必须在使用完后 release(&p->lock)
// - 设置其他进程属性（如 parent, name 等）
//
static struct proc*
allocproc(void)
{
  struct proc *p;

  // 遍历进程表，查找第一个 UNUSED 槽位
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);        // 获取进程锁
    if(p->state == UNUSED) {
      goto found;             // 找到空闲槽位
    } else {
      release(&p->lock);      // 不是空闲的，释放锁继续查找
    }
  }
  return 0;                   // 进程表已满

found:
  // 找到空闲槽位，开始初始化
  p->pid = allocpid();        // 分配唯一的 PID
  p->state = USED;            // 标记为"正在使用"（过渡状态）
  p->priority = 5;            // 设置默认优先级为 5（中等优先级，范围 0-9）
  p->errno = 0;               // 初始化 errno 为 0（无错误）
  
  // 初始化 MLFQ 调度器字段
  p->mlfq_level = 0;          // 新进程从最高优先级队列开始
  p->time_used = 0;           // 时间片使用清零
  p->time_quantum = 1;        // Level 0 的时间片 = 1 tick

  // 分配 trapframe 页
  // trapframe 用于保存用户态寄存器（trap 时使用）
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);              // 分配失败，清理并返回
    release(&p->lock);
    return 0;
  }

  // 创建空的用户页表
  // 此时只包含 trampoline 和 trapframe 映射，没有用户代码和数据
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);              // 分配失败，清理并返回
    release(&p->lock);
    return 0;
  }

  // 设置进程的初始上下文
  // 第一次被调度时会从这里开始执行
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;      // 返回地址设置为 forkret
                                        // 第一次调度时会跳转到 forkret
  p->context.sp = p->kstack + PGSIZE;   // 栈指针指向内核栈顶
                                        // PGSIZE = 4096，栈向下增长

  return p;                   // 返回新分配的进程，持有 p->lock
}


// freeproc - 释放进程占用的所有资源

//
// 前提条件：必须持有 p->lock
//
// 释放的资源：
// 1. trapframe 页
// 2. 用户页表和所有用户内存页
// 3. 清空所有进程字段
//
// 注意：不释放内核栈（它是在 proc_mapstacks 中统一分配的）
//
static void
freeproc(struct proc *p)
{
  // 释放 trapframe 页
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  
  // 释放用户页表和所有用户内存
  // COW fork: 使用引用计数，可能不会立即释放共享页
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  
  // 清空所有进程字段，恢复初始状态
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;          // 标记为未使用，可以被重新分配
}


// proc_pagetable - 为进程创建用户页表

//
// 创建的页表包含：
// 1. TRAMPOLINE 页：跳板代码（用户态/内核态切换）
// 2. TRAPFRAME 页：保存用户寄存器的数据页
// 
// 注意：此时还没有用户代码和数据，需要后续通过 exec 或 fork 填充
//
// 内存布局（用户视角）：
//   MAXVA        [Trampoline - 跳板代码，用户不可访问]
//   TRAPFRAME    [Trapframe  - 寄存器数据页]
//   ...          [用户代码和数据]
//   0            [起始地址]
//
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // 创建空的根页表（一页大小）
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // 映射 trampoline 页（用于系统调用返回）
  // 虚拟地址：TRAMPOLINE（用户地址空间的最高页）
  // 物理地址：trampoline 代码段
  // 权限：PTE_R | PTE_X（可读可执行，但不可写，且无 PTE_U）
  // 
  // 为什么没有 PTE_U？
  // - 只有 supervisor 模式可以访问
  // - 用户代码无法直接跳转到 trampoline
  // - 只在 trap 返回时由内核使用
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // 映射 trapframe 页（紧邻 trampoline 下方）
  // 虚拟地址：TRAPFRAME
  // 物理地址：p->trapframe（每个进程独立的物理页）
  // 权限：PTE_R | PTE_W（可读写，但无 PTE_U，用户不可访问）
  //
  // 为什么需要映射到用户页表？
  // - uservec 在切换页表前需要访问 trapframe
  // - 使用固定虚拟地址 TRAPFRAME，所有进程相同
  // - 但映射到不同的物理页，实现隔离
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}


// proc_freepagetable - 释放进程的页表和物理内存

//
// 释放步骤：
// 1. 取消 TRAMPOLINE 映射（不释放物理页，因为是内核代码）
// 2. 取消 TRAPFRAME 映射（不释放物理页，已在 freeproc 中释放）
// 3. 释放用户页表和所有用户内存页（uvmfree）
//
// 参数：
// - pagetable: 要释放的页表
// - sz: 用户内存大小（决定要释放多少页）
//
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);  // do_free=0，不释放 trampoline
  uvmunmap(pagetable, TRAPFRAME, 1, 0);   // do_free=0，trapframe 另外处理
  uvmfree(pagetable, sz);                 // 释放用户内存和页表
}


// userinit - 创建并初始化第一个用户进程

//
// 调用时机：系统启动时，在 main() 中调用
//
// 第一个用户进程的特殊性：
// - PID = 1（通常）
// - 没有父进程（parent = 0）
// - 通过 forkret → kexec("/init") 启动
// - init 进程是所有用户进程的祖先
//
// 后续流程：
// 1. scheduler 选中此进程
// 2. 切换到 forkret
// 3. forkret 调用 kexec("/init")
// 4. 加载并运行 /init 程序
//
void
userinit(void)
{
  struct proc *p;

  p = allocproc();            // 分配进程槽位（持有 p->lock）
  initproc = p;               // 保存为全局 init 进程指针
  
  p->cwd = namei("/");        // 设置当前目录为根目录

  p->state = RUNNABLE;        // 标记为可运行，等待调度

  release(&p->lock);          // 释放锁
}


// growproc - 增长或收缩进程的用户内存

//
// 用途：实现 sbrk() 系统调用
// - sbrk(n > 0): 扩大堆空间
// - sbrk(n < 0): 缩小堆空间
//
// 参数：
// - n: 要增加/减少的字节数
//   - n > 0: 扩大内存
//   - n < 0: 缩小内存
//   - n = 0: 只返回当前大小，不修改
//
// 返回值：
// - 0: 成功
// - -1: 失败（内存不足或地址空间冲突）
//
// 限制：
// - 不能增长到超过 TRAPFRAME 地址（避免与 trapframe 冲突）
//
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;                 // 当前进程大小
  
  if(n > 0){
    // 扩大内存
    if(sz + n > TRAPFRAME) {  // 检查是否会超过地址空间限制
      return -1;              // 超过限制，失败
    }
    
    // 调用 uvmalloc 分配新页面
    // PTE_W: 新分配的堆页面是可写的
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;              // 分配失败（内存不足）
    }
  } else if(n < 0){
    // 缩小内存
    sz = uvmdealloc(p->pagetable, sz, sz + n);  // 释放页面
  }
  
  p->sz = sz;                 // 更新进程大小
  return 0;
}


// kfork - 创建子进程（实现 fork 系统调用）

//
// 功能：复制当前进程，创建一个几乎相同的子进程
//
// 父子进程的区别：
// - PID 不同（子进程有新的 PID）
// - fork 返回值不同（父进程返回子 PID，子进程返回 0）
//
// 复制的内容：
// 1. 用户内存（代码、数据、堆、栈）- 使用 COW 优化
// 2. 寄存器状态（trapframe）
// 3. 打开的文件描述符（增加引用计数）
// 4. 当前工作目录
// 5. 进程名称
//
// COW 优化：
// - 不立即复制物理页面
// - 父子进程共享页面，标记为 COW
// - 写入时才真正复制（延迟复制）
//
// 返回值：
// - 父进程：返回子进程的 PID
// - 子进程：返回 0
// - 失败：返回 -1
//
int
kfork(void)
{
  int i, pid;
  struct proc *np;            // 新进程指针（子进程）
  struct proc *p = myproc();  // 当前进程指针（父进程）

  // 分配新进程结构体
  if((np = allocproc()) == 0){
    return -1;                // 进程表已满或内存不足
  }

  // 复制用户内存（COW 版本）
  // uvmcopy 现在会：
  // 1. 不分配新物理页
  // 2. 共享父进程的物理页
  // 3. 标记可写页为 COW
  // 4. 增加页面引用计数
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);             // 复制失败，清理子进程
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;             // 设置子进程的内存大小

  // 复制用户寄存器状态
  // 确保子进程恢复到与父进程相同的执行点
  *(np->trapframe) = *(p->trapframe);

  // 让 fork 在子进程中返回 0
  // 父进程的 a0 寄存器会在系统调用返回时被设置为子进程的 PID
  // 子进程的 a0 寄存器设置为 0
  np->trapframe->a0 = 0;

  // 复制打开的文件描述符
  // 父子进程共享打开的文件，增加引用计数
  // 这样父子进程可以独立关闭文件
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);  // 增加文件引用计数
  
  // 复制当前工作目录
  np->cwd = idup(p->cwd);     // 增加 inode 引用计数

  // 复制进程名称（用于调试）
  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;              // 保存子进程 PID（用于返回）

  release(&np->lock);         // 释放子进程锁

  // 设置父子关系
  // 使用 wait_lock 保护（避免与 wait/exit 竞争）
  acquire(&wait_lock);
  np->parent = p;             // 设置父进程指针
  release(&wait_lock);

  // 将子进程标记为可运行
  acquire(&np->lock);
  np->state = RUNNABLE;       // 现在可以被调度器选中了
  int child_priority = np->priority;  // 保存子进程优先级（避免重复加锁）
  int child_level = np->mlfq_level;   // 保存 MLFQ 级别
  release(&np->lock);
  
  // MLFQ 调度器：将新进程加入队列
  // 检查当前是否使用 MLFQ 调度器
  extern struct proc* (*select_next_proc)(void);
  extern struct proc* mlfq_scheduler(void);
  if(select_next_proc == mlfq_scheduler) {
    mlfq_add_process(np, child_level);  // 加入对应级别的队列
  }

  // 优先级抢占：如果子进程优先级更高，主动让出 CPU
  // 这样高优先级的子进程可以立即运行，而不是等待时钟中断
  if(child_priority > p->priority) {
    yield();                  // 立即重新调度
  }

  return pid;                 // 父进程中返回子进程的 PID
}


// reparent - 将进程的所有子进程过继给 init 进程

//
// 调用时机：进程退出时（在 kexit 中调用）
//
// 前提条件：调用者必须持有 wait_lock
//
// 为什么需要过继？
// - 避免孤儿进程（父进程已退出，但子进程还在运行）
// - Unix 约定：所有孤儿进程由 init 进程接管
// - init 进程会周期性调用 wait 回收僵尸子进程
//
// 流程：
// 1. 遍历进程表
// 2. 找到所有 parent == p 的进程
// 3. 将它们的 parent 改为 initproc
// 4. 如果有僵尸进程，唤醒 init 去回收
//
void
reparent(struct proc *p)
{
  struct proc *pp;

  // 遍历所有进程，查找子进程
  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;  // 过继给 init 进程
      wakeup(initproc);       // 唤醒 init（如果它在 wait 中睡眠）
    }
  }
}


// kexit - 终止当前进程

//
// 特点：此函数永不返回！
//
// 进程退出流程：
// 1. 关闭所有打开的文件
// 2. 释放当前工作目录
// 3. 将子进程过继给 init
// 4. 唤醒父进程（如果在 wait 中）
// 5. 设置状态为 ZOMBIE
// 6. 跳转到调度器（永不返回）
//
// 为什么不直接释放资源？
// - 父进程需要通过 wait() 获取退出状态
// - 进程在 wait() 之前保持僵尸状态
// - 父进程调用 wait() 后才真正释放进程资源
//
// 僵尸进程 (Zombie)：
// - 已退出但未被回收的进程
// - 占用进程表槽位，但不占用其他资源
// - 必须由父进程 wait() 才能完全释放
//
void
kexit(int status)
{
  struct proc *p = myproc();

  // init 进程不能退出（系统会崩溃）
  if(p == initproc)
    panic("init exiting");

  // 关闭所有打开的文件
  // 减少文件引用计数，可能触发文件关闭
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);           // 关闭文件，减少引用计数
      p->ofile[fd] = 0;       // 清空文件描述符槽位
    }
  }

  // 释放当前工作目录的 inode
  // begin_op/end_op 确保文件系统操作的原子性
  begin_op();
  iput(p->cwd);               // 减少 inode 引用计数
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);        // 获取 wait 锁（保护父子关系）

  // 将所有子进程过继给 init 进程
  reparent(p);

  // 唤醒父进程（如果父进程在 wait() 中睡眠）
  wakeup(p->parent);
  
  acquire(&p->lock);          // 获取进程锁（sched 要求）
  
  // MLFQ 调度器：退出前从队列移除
  extern struct proc* (*select_next_proc)(void);
  extern struct proc* mlfq_scheduler(void);
  if(select_next_proc == mlfq_scheduler) {
    mlfq_remove_process(p, p->mlfq_level);
  }

  p->xstate = status;         // 保存退出状态码（传给 wait）
  p->state = ZOMBIE;          // 设置为僵尸状态

  release(&wait_lock);        // 释放 wait 锁

  // 跳转到调度器，永不返回
  // sched() 会切换到调度器上下文
  // 调度器会选择其他进程运行
  // 此进程的 PCB 将保持 ZOMBIE 状态，直到父进程 wait
  sched();
  
  panic("zombie exit");       // 不应该执行到这里
}


// kwait - 等待子进程退出

//
// 功能：阻塞等待任意一个子进程退出，并回收其资源
//
// 参数：
// - addr: 用户空间地址，用于返回子进程的退出状态
//   - 如果为 0，不返回退出状态
//   - 如果非 0，将 xstate 写入此地址
//
// 返回值：
// - 成功：返回退出的子进程 PID
// - 失败：返回 -1（没有子进程或被杀死）
//
// 等待逻辑：
// 1. 扫描进程表查找僵尸子进程
// 2. 如果找到，回收并返回
// 3. 如果没有僵尸子进程但有活着的子进程，睡眠等待
// 4. 如果没有子进程，返回 -1
//
// 为什么需要循环？
// - 可能被虚假唤醒
// - 被唤醒时子进程可能还没完全退出
//
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);        // 获取 wait 锁（保护父子关系）

  for(;;){
    // 扫描进程表，查找已退出的子进程
    havekids = 0;             // 是否有子进程（活着的或僵尸的）
    
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){    // 找到子进程
        // 确保子进程不在 exit() 或 swtch() 中
        acquire(&pp->lock);

        havekids = 1;         // 标记"有子进程"
        
        if(pp->state == ZOMBIE){
          // 找到一个僵尸子进程！
          pid = pp->pid;      // 保存 PID（用于返回）
          
          // 如果 addr 非零，将退出状态复制到用户空间
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;        // copyout 失败
          }
          
          // 释放子进程的所有资源
          freeproc(pp);       // 释放内存、页表等
          release(&pp->lock);
          release(&wait_lock);
          return pid;         // 返回子进程 PID
        }
        
        release(&pp->lock);
      }
    }

    // 没有找到僵尸子进程
    if(!havekids || killed(p)){
      // 情况1：根本没有子进程
      // 情况2：当前进程被杀死
      release(&wait_lock);
      return -1;
    }
    
    // 有子进程，但都还活着，睡眠等待
    // 当子进程 exit 时会 wakeup(p->parent)
    sleep(p, &wait_lock);     // 在 wait_lock 上睡眠
                              // sleep 会释放 wait_lock，唤醒后重新获取
  }
}


// scheduler - 每个 CPU 的进程调度器主循环

//
// 特点：
// - 每个 CPU 启动后都会调用此函数
// - 永不返回！无限循环选择和运行进程
//
// 调度框架（策略模式）：
// - 使用可插拔的调度策略（通过函数指针 select_next_proc）
// - 策略只负责"选择进程"
// - 框架负责"切换和运行进程"
//
// 调度流程：
// 1. 调用 select_next_proc() 选择下一个进程
// 2. 检查进程状态（必须是 RUNNABLE）
// 3. 切换到选中的进程
// 4. 进程运行一段时间后 yield/sleep/exit
// 5. 切换回调度器，重复步骤 1
//
// 锁的微妙之处：
// - 调度器获取 p->lock，然后 swtch
// - 进程第一次运行时从 forkret 开始，forkret 释放 p->lock
// - 进程 yield/sleep 时先 acquire p->lock，然后 swtch 回调度器
// - 调度器 swtch 返回后在循环中 release p->lock
//
// 中断处理：
// - 每轮循环开启中断（避免死锁）
// - 立即关闭中断（避免竞争条件）
// - 在 swtch 到进程后，进程自己决定何时开中断
//
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;                // 调度器开始时没有运行进程
  
  for(;;){
    // 最近运行的进程可能关闭了中断
    // 开启中断避免所有进程都在等待时死锁
    // 然后立即关闭，避免 wfi 前的竞争条件
    intr_on();                // 开启中断
    intr_off();               // 关闭中断

    // 调用当前的调度策略选择下一个进程
    // 策略函数会遍历进程表，根据算法选择最佳进程
    p = select_next_proc();
    
    if(p != 0) {
      // 策略返回了一个候选进程
      acquire(&p->lock);      // 获取进程锁
      
      // 二次检查状态（防止竞争条件）
      // 进程可能在策略选择后被其他 CPU 运行了
      if(p->state == RUNNABLE) {
        // 切换到此进程
        // 进程的职责：
        // 1. 在返回调度器前释放 p->lock
        // 2. 在返回调度器前重新获取 p->lock
        // 这确保调度器可以安全地在循环中释放锁
        p->state = RUNNING;   // 标记为运行状态
        c->proc = p;          // 设置当前 CPU 运行的进程
        
        // 上下文切换：从调度器切换到进程
        // 保存：c->context（调度器的 ra, sp, s0-s11）
        // 恢复：p->context（进程的 ra, sp, s0-s11）
        // 然后跳转到 p->context.ra（通常是 forkret 或 sched 的返回点）
        swtch(&c->context, &p->context);

        // 当进程再次 swtch 回来时，从这里继续执行
        // 此时进程已经运行了一段时间（可能 yield/sleep/exit）
        // 进程应该已经改变了 p->state（不再是 RUNNING）
        c->proc = 0;          // 清空当前进程指针
      }
      
      release(&p->lock);      // 释放进程锁
    } else {
      // 策略返回 NULL，表示没有可运行的进程
      // 进入低功耗等待状态，直到中断到来
      // wfi (Wait For Interrupt): RISC-V 指令，暂停 CPU 直到中断
      asm volatile("wfi");
    }
  }
}

// default_round_robin - 默认的轮转调度策略
//
// 算法：简单轮转 (Round-Robin)
// - 按照进程表顺序遍历
// - 返回第一个 RUNNABLE 进程
// - 公平性：所有进程机会均等
// - 时间复杂度：O(n)
//
// 优点：
// - 实现简单
// - 公平（无饥饿）
// - 适合分时系统
//
// 缺点：
// - 不考虑优先级
// - 不考虑进程特性（I/O 密集 vs CPU 密集）
// - 可能导致护航效应（convoy effect）
//
static struct proc*
default_round_robin(void)
{
  struct proc *p;
  
  // 线性遍历进程表
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);        // 临时获取锁检查状态
    
    if(p->state == RUNNABLE) {
      release(&p->lock);      // 释放锁（scheduler 会重新获取）
      return p;               // 找到第一个可运行进程
    }
    
    release(&p->lock);
  }
  
  return 0;                   // 没有可运行的进程
}


// set_scheduler - 设置调度策略
//
// 功能：运行时动态切换调度算法
//
// 参数：
// - selector: 新的调度策略函数指针
//   - 如果为 NULL，恢复默认轮转调度
//
// 使用示例：
//   extern struct proc* priority_scheduler(void);
//   set_scheduler(priority_scheduler);  // 切换到优先级调度
//
// 线程安全：
// - 直接修改函数指针（原子操作）
// - 调度器会在下一轮循环使用新策略
//
// 注意：
// - 切换时机：任何时候都可以切换
// - 立即生效：下一轮调度循环就会使用新策略
// - 无需重启：动态切换，适合性能测试和对比
//
void
set_scheduler(struct proc* (*selector)(void))
{
  if(selector == 0) {
    select_next_proc = default_round_robin;  // 恢复默认
  } else {
    select_next_proc = selector;             // 切换到新策略
  }
}

// get_scheduler_name - 获取当前调度器名称（调试用）
//
// 返回值：当前调度策略的描述字符串
//
// 用途：
// - 调试输出
// - 性能测试时记录使用的算法
//
const char*
get_scheduler_name(void)
{
  if(select_next_proc == default_round_robin) {
    return "Round-Robin";
  }
  // 其他调度器由 scheduler_ext.c 提供识别函数
  return "Custom";
}


// sched - 切换到调度器

//
// 调用条件（全部检查，违反则 panic）：
// 1. 必须持有 p->lock（且只持有这一个锁）
// 2. 必须已经改变了 p->state（不能是 RUNNING）
// 3. 中断必须关闭
//
// 为什么保存/恢复 intena？
// - intena 是内核线程的属性，不是 CPU 的属性
// - 理论上应该是 p->intena，但那样会使代码复杂化
// - 目前的设计：在少数持有锁但没有进程的情况下有效
//
// intena 的作用：
// - 记录进程在获取第一个锁前的中断状态
// - 释放最后一个锁时恢复到此状态
// - 确保中断状态在进程切换前后保持一致
//
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  // 安全性检查：确保调用条件满足
  if(!holding(&p->lock))
    panic("sched p->lock");   // 必须持有进程锁
    
  if(mycpu()->noff != 1)
    panic("sched locks");     // 必须只持有一个锁（p->lock）
                              // noff=1 表示只有一层 push_off
    
  if(p->state == RUNNING)
    panic("sched RUNNING");   // 状态必须已改变（不能还是 RUNNING）
    
  if(intr_get())
    panic("sched interruptible");  // 中断必须关闭

  // 保存当前的 intena（进程的中断状态）
  intena = mycpu()->intena;
  
  // 上下文切换：从进程切换回调度器
  // 保存：p->context（进程的内核执行状态）
  // 恢复：mycpu()->context（调度器的执行状态）
  // 跳转：到调度器的 swtch 返回点（scheduler 函数中）
  swtch(&p->context, &mycpu()->context);
  
  // 当进程再次被调度时，从这里继续执行
  // 恢复进程的 intena
  mycpu()->intena = intena;
}


// yield - 主动让出 CPU

//
// 用途：
// 1. 时间片用完（时钟中断调用）
// 2. 等待资源（轮询时主动让出）
// 3. 合作式多任务（进程主动让出）
//
// 实现：
// 1. 获取进程锁
// 2. 改变状态为 RUNNABLE（重新进入就绪队列）
// 3. 调用 sched() 切换到调度器
// 4. 调度器选择其他进程（或重新选择此进程）
// 5. 当再次被调度时，从 sched() 返回
// 6. 释放进程锁，继续执行
//
// 为什么先 acquire 再 sched？
// - sched() 要求持有 p->lock
// - 状态改变和切换必须是原子的
// - 防止在改变状态后、切换前被调度器看到不一致的状态
//
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);          // 获取进程锁
  
  // MLFQ 调度器：从当前队列移除（因为要重新调度）
  extern struct proc* (*select_next_proc)(void);
  extern struct proc* mlfq_scheduler(void);
  if(select_next_proc == mlfq_scheduler) {
    mlfq_remove_process(p, p->mlfq_level);
  }
  
  p->state = RUNNABLE;        // 改变状态为可运行
  
  // MLFQ 调度器：重新加入队列（保持同一级别）
  if(select_next_proc == mlfq_scheduler) {
    mlfq_add_process(p, p->mlfq_level);
  }
  
  sched();                    // 切换到调度器
  release(&p->lock);          // 切换回来后释放锁
}


// forkret - fork 的子进程第一次被调度时的入口点

//
// 调用路径：
// 1. allocproc() 设置 p->context.ra = forkret
// 2. scheduler() 第一次 swtch 到新进程
// 3. swtch 返回到 forkret（因为 ra 指向这里）
// 4. forkret 执行初始化工作
// 5. 返回到用户空间
//
// 第一个进程的特殊处理：
// - first = 1 时，初始化文件系统
// - 调用 kexec("/init") 加载 init 程序
// - 之后的进程跳过此步骤
//
// 为什么要在 forkret 中初始化文件系统？
// - 文件系统初始化需要调用 sleep()
// - sleep() 需要进程上下文
// - main() 中还没有进程上下文
//
// 返回方式：
// - 不是通过 ret 指令返回
// - 直接跳转到 trampoline.S 中的 userret
// - 模拟 usertrap() 的返回流程
//
void
forkret(void)
{
  extern char userret[];      // trampoline.S 中的 userret 标签
  static int first = 1;       // 静态变量，只有第一个进程为 1
  struct proc *p = myproc();

  // 此时仍持有 p->lock（从 scheduler 继承）
  // 必须释放，否则死锁
  release(&p->lock);

  if (first) {
    // 第一个进程的特殊初始化
    
    // 初始化文件系统
    // 必须在进程上下文中运行（因为会调用 sleep）
    // 不能在 main() 中运行
    fsinit(ROOTDEV);

    first = 0;
    // 确保其他 CPU 核心看到 first=0
    // 内存屏障，防止编译器或 CPU 重排序
    __sync_synchronize();

    // 现在可以调用 kexec() 了（文件系统已初始化）
    // 加载并执行 /init 程序
    // 将返回值（argc）放入 a0 寄存器
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");          // exec 失败，系统无法启动
    }
  }

  // 返回用户空间，模拟 usertrap() 的返回流程
  prepare_return();           // 设置 sstatus 和 sepc 寄存器
  
  uint64 satp = MAKE_SATP(p->pagetable);  // 构造用户页表的 satp 值
  
  // 计算 userret 在 trampoline 中的虚拟地址
  // userret 和 trampoline 都在同一页，使用相对偏移
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  
  // 跳转到 userret，传入 satp（用于切换页表）
  // 这是一个函数指针调用，跳转到 trampoline.S 中的 userret
  ((void (*)(uint64))trampoline_userret)(satp);
}


// sleep - 在条件变量上睡眠

//
// 用途：等待某个事件发生（如 I/O 完成、资源可用）
//
// 参数：
// - chan: 睡眠通道，通常是某个数据结构的地址
//   - 用作"等待事件"的标识符
//   - wakeup(chan) 会唤醒所有睡眠在此通道的进程
// - lk: 条件锁，保护等待条件的锁
//   - 调用者在调用 sleep 前必须持有此锁
//   - sleep 会释放此锁，唤醒后重新获取
//
// 典型使用模式：
//   acquire(&lock);
//   while(!condition) {
//       sleep(&condition, &lock);  // 等待条件满足
//   }
//   // condition 现在为真
//   release(&lock);
//
// 为什么需要 lk 参数？
// - 避免"丢失唤醒"问题（lost wakeup）
// - 确保在检查条件和睡眠之间的原子性
//
// 详细流程：
// 1. 持有 lk，检查条件为假
// 2. 调用 sleep(chan, lk)
// 3. sleep 获取 p->lock，然后释放 lk
// 4. 设置 chan 和 SLEEPING，调用 sched()
// 5. 被 wakeup 唤醒，sched 返回
// 6. 清空 chan，释放 p->lock，重新获取 lk
// 7. 返回调用者，重新检查条件
//
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // 为什么必须先获取 p->lock？
  // - 改变 p->state 需要持有 p->lock
  // - 防止在改变状态后、sched 前被其他 CPU 唤醒
  //
  // 为什么可以释放 lk？
  // - 一旦持有 p->lock，就不会错过 wakeup
  // - wakeup 必须获取 p->lock 才能改变状态
  // - 所以在持有 p->lock 后释放 lk 是安全的
  
  acquire(&p->lock);          // 获取进程锁 (DOC: sleeplock1)
  release(lk);                // 释放条件锁

  // MLFQ 调度器：睡眠前从队列移除
  extern struct proc* (*select_next_proc)(void);
  extern struct proc* mlfq_scheduler(void);
  if(select_next_proc == mlfq_scheduler) {
    mlfq_remove_process(p, p->mlfq_level);
  }

  // 进入睡眠状态
  p->chan = chan;             // 记录睡眠通道（wakeup 用此识别）
  p->state = SLEEPING;        // 改变状态为睡眠

  sched();                    // 切换到调度器（让出 CPU）

  // 被 wakeup 唤醒后，从这里继续执行
  // 清理睡眠状态
  p->chan = 0;                // 清空睡眠通道

  // MLFQ 调度器：I/O 操作后提升优先级（奖励交互式进程）
  if(select_next_proc == mlfq_scheduler) {
    // 提升一级（但不超过 level 0）
    if(p->mlfq_level > 0) {
      p->mlfq_level--;                    // 提升优先级
      p->time_quantum = 1 << p->mlfq_level;  // 更新时间片
      p->time_used = 0;                   // 重置时间片使用
    }
    // 重新加入队列（可能是更高的级别）
    mlfq_add_process(p, p->mlfq_level);
  }

  // 重新获取原来的锁
  // 这样调用者可以安全地重新检查条件
  release(&p->lock);          // 释放进程锁
  acquire(lk);                // 重新获取条件锁
}


// wakeup - 唤醒所有睡眠在指定通道上的进程

//
// 参数：
// - chan: 睡眠通道（某个数据结构的地址）
//
// 前提条件：
// - 调用者应该持有保护 chan 的条件锁
// - 确保在 wakeup 和进程睡眠之间不会有竞争
//
// 实现：
// - 遍历所有进程
// - 找到 SLEEPING 且 chan 匹配的进程
// - 改变状态为 RUNNABLE（重新进入就绪队列）
//
// 为什么跳过 myproc()？
// - 进程不会唤醒自己
// - 避免不必要的锁开销
//
// 广播语义：
// - 唤醒**所有**等待此通道的进程
// - 不是只唤醒一个（那是 signal，不是 broadcast）
// - 被唤醒的进程会重新竞争锁和检查条件
//
void
wakeup(void *chan)
{
  struct proc *p;

  // 遍历所有进程
  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){        // 跳过当前进程
      acquire(&p->lock);      // 获取进程锁
      
      // 检查是否匹配睡眠通道
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;  // 唤醒：改为可运行状态
      }
      
      release(&p->lock);      // 释放进程锁
    }
  }
}


// kkill - 杀死指定 PID 的进程

//
// 功能：设置进程的 killed 标志
//
// 注意：不会立即终止进程！
// - 只设置 killed = 1
// - 进程在返回用户态时检查此标志
// - 进程在 usertrap() 中会调用 exit(-1)
//
// 为什么不立即杀死？
// - 进程可能持有锁或在关键区
// - 需要给进程机会清理资源
// - 避免内核状态不一致
//
// 特殊处理：
// - 如果进程在 SLEEPING，唤醒它
// - 这样它可以更快地检查 killed 标志并退出
//
// 查找方式：
// - 线性扫描进程表，O(NPROC)
// - 优化建议：使用哈希表可降到 O(1)
//
int
kkill(int pid)
{
  struct proc *p;

  // 遍历进程表查找目标进程
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    
    if(p->pid == pid){
      // 找到目标进程
      p->killed = 1;          // 设置 killed 标志
      
      if(p->state == SLEEPING){
        // 如果进程在睡眠，唤醒它
        // 让它有机会尽快退出
        p->state = RUNNABLE;
      }
      
      release(&p->lock);
      return 0;               // 成功
    }
    
    release(&p->lock);
  }
  
  return -1;                  // 未找到指定 PID 的进程
}


// setkilled - 设置进程的 killed 标志

//
// 线程安全版本的设置接口
// 使用锁保护，确保原子性
//
void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}


// killed - 检查进程是否被杀死

//
// 线程安全版本的查询接口
// 使用锁保护，确保读取的一致性
//
// 返回值：
// - 1: 进程应该退出
// - 0: 进程可以继续运行
//
int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;              // 读取 killed 标志
  release(&p->lock);
  return k;
}


// either_copyout - 通用复制函数（内核 → 用户或内核）

//
// 用途：系统调用中返回数据时使用
//
// 参数：
// - user_dst: 目标是用户空间(1)还是内核空间(0)
// - dst: 目标地址（虚拟地址）
// - src: 源数据（内核指针）
// - len: 复制长度
//
// 为什么需要区分？
// - 用户地址需要通过页表转换（copyout）
// - 内核地址可以直接访问（memmove）
//
// 安全性：
// - copyout 会检查目标页的权限
// - 防止写入只读页或内核页
//
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    // 目标是用户空间，使用 copyout
    // copyout 会：
    // 1. 通过页表转换虚拟地址
    // 2. 检查页面权限
    // 3. 处理 COW 页面
    return copyout(p->pagetable, dst, src, len);
  } else {
    // 目标是内核空间，直接内存复制
    memmove((char *)dst, src, len);
    return 0;
  }
}


// either_copyin - 通用复制函数（用户或内核 → 内核）

//
// 用途：系统调用中读取参数时使用
//
// 参数：
// - dst: 目标缓冲区（内核指针）
// - user_src: 源是用户空间(1)还是内核空间(0)
// - src: 源地址（虚拟地址）
// - len: 复制长度
//
// 安全性：
// - copyin 会检查源页的权限
// - 防止读取内核页（权限提升漏洞）
//
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    // 源是用户空间，使用 copyin
    // copyin 会通过页表转换并检查权限
    return copyin(p->pagetable, dst, src, len);
  } else {
    // 源是内核空间，直接内存复制
    memmove(dst, (char*)src, len);
    return 0;
  }
}


// procdump - 打印进程列表（调试用）

//
// 触发方式：
// - 用户在控制台按 Ctrl+P
// - 内核代码中手动调用
//
// 注意：不使用锁！
// - 原因：避免在系统卡死时进一步阻塞
// - 后果：可能读到不一致的数据（但只是调试信息）
//
// 输出格式：
//   PID  STATE   NAME
//   1    run     init
//   2    sleep   sh
//   ...
//
void
procdump(void)
{
  // 进程状态的字符串表示
  static char *states[] = {
  [UNUSED]    "unused",       // 未使用
  [USED]      "used",         // 正在初始化
  [SLEEPING]  "sleep ",       // 睡眠中
  [RUNNABLE]  "runble",       // 就绪（可运行）
  [RUNNING]   "run   ",       // 运行中
  [ZOMBIE]    "zombie"        // 僵尸（已退出）
  };
  struct proc *p;
  char *state;

  printf("\n");
  
  // 遍历所有进程（不加锁，可能不一致）
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;               // 跳过未使用的槽位
      
    // 获取状态字符串
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";          // 未知状态
    
    // 打印：PID 状态 名称
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

