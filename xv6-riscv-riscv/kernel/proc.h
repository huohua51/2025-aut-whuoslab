// 进程上下文 (Context) 结构体
// 
// 用途：保存内核线程切换时需要保存/恢复的寄存器
// 
// 为什么只保存这些寄存器？
// - 遵循 RISC-V 调用约定 (Calling Convention)
// - ra, sp: 必须保存（返回地址和栈指针）
// - s0-s11: callee-saved 寄存器，被调用函数必须保证这些寄存器不变
// - 其他寄存器 (a0-a7, t0-t6): caller-saved，由编译器在调用前自动保存
//
// 使用场景：
// - 进程 A 调用 sched() → 保存到 p->context
// - 调度器选择进程 B → 从 p->context 恢复
// - 进程 B 继续执行
//
struct context {
  uint64 ra;      // 返回地址 (Return Address)：函数返回时跳转的 PC
  uint64 sp;      // 栈指针 (Stack Pointer)：指向当前栈顶

  // 以下是 RISC-V callee-saved 寄存器 (被调用者保存寄存器)
  // 根据 RISC-V 调用约定，这些寄存器在函数调用前后必须保持不变
  // 因此上下文切换时必须保存这些寄存器
  uint64 s0;      // Saved register 0 / Frame Pointer
  uint64 s1;      // Saved register 1
  uint64 s2;      // Saved register 2
  uint64 s3;      // Saved register 3
  uint64 s4;      // Saved register 4
  uint64 s5;      // Saved register 5
  uint64 s6;      // Saved register 6
  uint64 s7;      // Saved register 7
  uint64 s8;      // Saved register 8
  uint64 s9;      // Saved register 9
  uint64 s10;     // Saved register 10
  uint64 s11;     // Saved register 11
};


// 每个 CPU 核心的状态 (Per-CPU State)

//
// 设计原因：
// - 多核系统中，每个 CPU 核心需要独立的状态
// - 避免核心间的资源竞争和同步开销
//
// 关键用途：
// - 快速获取当前运行的进程（c->proc）
// - 保存调度器的上下文（c->context）
// - 管理中断状态（noff, intena）
//
struct cpu {
  struct proc *proc;          // 当前在这个 CPU 上运行的进程指针
                              // 如果为 NULL，表示 CPU 正在运行调度器
                              
  struct context context;     // 调度器的上下文
                              // swtch(&p->context, &c->context) 会切换到这里
                              // 即：从进程切换回调度器
                              
  int noff;                   // push_off() 的嵌套深度
                              // 用于实现可重入的锁机制
                              // 每次 push_off() +1，pop_off() -1
                              // 当 noff 降为 0 时才真正开启中断
                              
  int intena;                 // 在第一次 push_off() 之前，中断是否开启？
                              // 保存原始中断状态，用于 pop_off() 恢复
                              // 1 = 开启，0 = 关闭
};

extern struct cpu cpus[NCPU];  // 所有 CPU 核心的数组（最多 NCPU 个核心）


// Trapframe 结构体

//
// 用途：在用户态陷入内核态（trap）时，保存用户态的所有寄存器状态
//
// 内存布局：
// - 每个进程有独立的 trapframe 物理页
// - 在用户页表中映射到固定虚拟地址 TRAPFRAME
// - 在内核页表中不特殊映射（通过物理地址访问）
//
// 工作流程：
// 1. 用户态发生 trap（系统调用/中断/异常）
// 2. CPU 跳转到 trampoline.S 中的 uservec
// 3. uservec 保存所有 32 个通用寄存器到 trapframe
// 4. uservec 从 trapframe 加载内核栈指针、页表等
// 5. 切换到内核页表，调用 usertrap()
// 6. 返回时通过 userret 从 trapframe 恢复所有寄存器
//
// 为什么 trapframe 包含 callee-saved 寄存器？
// - 因为返回路径（usertrapret → userret）不经过完整的内核调用栈
// - 直接从 trapframe 恢复，绕过了正常的函数返回流程
//
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // 内核页表的 satp 值
                                   // 用于从用户页表切换到内核页表
                                   
  /*   8 */ uint64 kernel_sp;     // 进程的内核栈顶地址
                                   // trap 时切换到这个栈
                                   
  /*  16 */ uint64 kernel_trap;   // usertrap() 函数的地址
                                   // uservec 会跳转到这里
                                   
  /*  24 */ uint64 epc;            // 保存的用户程序计数器 (PC)
                                   // 返回用户态时恢复到这个地址
                                   
  /*  32 */ uint64 kernel_hartid; // 保存的内核 tp 寄存器（CPU ID）
                                   // 用于多核系统识别 CPU 核心
  
  // 以下是用户态的 32 个通用寄存器
  // 按照 RISC-V 寄存器编号顺序排列
  /*  40 */ uint64 ra;     // x1:  返回地址
  /*  48 */ uint64 sp;     // x2:  栈指针
  /*  56 */ uint64 gp;     // x3:  全局指针 (Global Pointer)
  /*  64 */ uint64 tp;     // x4:  线程指针 (Thread Pointer)
  /*  72 */ uint64 t0;     // x5:  临时寄存器 0
  /*  80 */ uint64 t1;     // x6:  临时寄存器 1
  /*  88 */ uint64 t2;     // x7:  临时寄存器 2
  /*  96 */ uint64 s0;     // x8:  保存寄存器 0 / 帧指针 (Frame Pointer)
  /* 104 */ uint64 s1;     // x9:  保存寄存器 1
  /* 112 */ uint64 a0;     // x10: 函数参数 0 / 返回值 0
  /* 120 */ uint64 a1;     // x11: 函数参数 1 / 返回值 1
  /* 128 */ uint64 a2;     // x12: 函数参数 2
  /* 136 */ uint64 a3;     // x13: 函数参数 3
  /* 144 */ uint64 a4;     // x14: 函数参数 4
  /* 152 */ uint64 a5;     // x15: 函数参数 5
  /* 160 */ uint64 a6;     // x16: 函数参数 6
  /* 168 */ uint64 a7;     // x17: 函数参数 7
  /* 176 */ uint64 s2;     // x18: 保存寄存器 2
  /* 184 */ uint64 s3;     // x19: 保存寄存器 3
  /* 192 */ uint64 s4;     // x20: 保存寄存器 4
  /* 200 */ uint64 s5;     // x21: 保存寄存器 5
  /* 208 */ uint64 s6;     // x22: 保存寄存器 6
  /* 216 */ uint64 s7;     // x23: 保存寄存器 7
  /* 224 */ uint64 s8;     // x24: 保存寄存器 8
  /* 232 */ uint64 s9;     // x25: 保存寄存器 9
  /* 240 */ uint64 s10;    // x26: 保存寄存器 10
  /* 248 */ uint64 s11;    // x27: 保存寄存器 11
  /* 256 */ uint64 t3;     // x28: 临时寄存器 3
  /* 264 */ uint64 t4;     // x29: 临时寄存器 4
  /* 272 */ uint64 t5;     // x30: 临时寄存器 5
  /* 280 */ uint64 t6;     // x31: 临时寄存器 6
};


// 进程状态枚举 (Process State)

//
// 进程的生命周期状态转换：
//
//   UNUSED → USED → RUNNABLE → RUNNING → ZOMBIE → UNUSED
//              ↓       ↑  ↓                ↑
//              └→ SLEEPING ←───────────────┘
//
// 状态说明：
// - UNUSED (0):   进程槽未使用，可以分配给新进程
// - USED (1):     进程槽已分配，正在初始化中（过渡状态）
// - SLEEPING (2): 进程睡眠，等待某个事件（如 I/O 完成）
// - RUNNABLE (3): 进程就绪，等待被调度器选中运行
// - RUNNING (4):  进程正在某个 CPU 上运行
// - ZOMBIE (5):   进程已退出，等待父进程回收（wait）
//
enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };


// 进程控制块 (Process Control Block, PCB)

//
// 这是操作系统中最核心的数据结构之一，包含进程的所有状态信息
//
// 字段分组说明：
// 1. 同步字段：需要持有相应的锁才能访问
// 2. 私有字段：进程独占，无需加锁
//
struct proc {
  struct spinlock lock;        // 保护此进程状态的自旋锁
                               // 必须在修改进程状态前获取此锁
                               // 防止多个 CPU 同时修改同一进程的状态

  //  需要持有 p->lock 才能访问的字段 
  // 这些字段可能被多个 CPU 同时访问，必须加锁保护
  
  enum procstate state;        // 进程当前状态 (UNUSED/USED/SLEEPING/RUNNABLE/RUNNING/ZOMBIE)
                               // 调度器根据此字段决定是否运行此进程
                               
  void *chan;                  // 睡眠通道 (Sleep Channel)
                               // 非零时表示进程正在此地址上睡眠
                               // wakeup(chan) 会唤醒所有睡眠在此地址的进程
                               // 通常用地址作为"等待事件"的标识符
                               
  int killed;                  // 进程是否被杀死
                               // 非零表示进程应该尽快退出
                               // 进程在返回用户态时会检查此标志并退出
                               
  int xstate;                  // 进程退出状态码
                               // 传递给父进程的 wait() 系统调用
                               // 通常 0 表示成功，非零表示错误
                               
  int pid;                     // 进程 ID (Process ID)
                               // 系统中每个进程的唯一标识符
                               // 从 1 开始递增分配
                               
  int priority;                // 进程调度优先级
                               // 范围：0-9 (数字越大优先级越高)
                               // 默认值：5 (中等优先级)
                               // 用于优先级调度器
                               
  int errno;                   // 最后一次系统调用的错误码
                               // 0 表示成功，非零表示错误
                               // 兼容 POSIX errno 机制
  
  // MLFQ 调度器相关字段
  int mlfq_level;              // 当前所在的 MLFQ 队列级别
                               // 范围：0-4 (0 最高优先级，4 最低)
                               // 新进程从 level 0 开始
                               
  int time_used;               // 当前时间片已使用的 tick 数
                               // 用于跟踪进程运行时间
                               // 达到 time_quantum 时降级
                               
  int time_quantum;            // 当前级别的时间片配额
                               // Level 0: 1 tick
                               // Level 1: 2 ticks
                               // Level 2: 4 ticks
                               // Level 3: 8 ticks
                               // Level 4: 16 ticks

  //  需要持有 wait_lock 才能访问的字段 
  // wait_lock 保护父子关系，必须在 p->lock 之前获取（避免死锁）
  
  struct proc *parent;         // 父进程指针
                               // 用于进程退出时通知父进程
                               // 孤儿进程会被重新指向 init 进程

  //  进程私有字段，无需加锁 
  // 这些字段只被进程自己访问，不会有并发问题
  
  uint64 kstack;               // 内核栈的虚拟地址
                               // 每个进程有独立的内核栈（1 页 = 4KB）
                               // 用于处理系统调用和中断时的内核代码执行
                               // 布局：高地址有一个 guard page（无映射，用于检测栈溢出）
                               
  uint64 sz;                   // 进程用户内存大小（字节数）
                               // 表示进程的地址空间从 0 到 sz
                               // 通过 sbrk() 系统调用可以增长或收缩
                               
  pagetable_t pagetable;       // 用户页表指针
                               // 指向进程的根页表（Page Directory）
                               // 包含用户空间的虚拟地址到物理地址的映射
                               // 每个进程有独立的页表，实现地址空间隔离
                               
  struct trapframe *trapframe; // 指向 trapframe 页的指针
                               // 用于用户态/内核态切换时保存用户寄存器
                               // 物理上是一个独立的页面
                               // 在用户页表中映射到 TRAPFRAME 虚拟地址
                               
  struct context context;      // 进程的内核线程上下文
                               // swtch(&p->context, &c->context) 时保存/恢复
                               // 用于进程切换时保存内核执行状态
                               
  struct file *ofile[NOFILE];  // 打开的文件描述符数组
                               // ofile[fd] 指向对应的 file 结构体
                               // fd 0: 标准输入，fd 1: 标准输出，fd 2: 标准错误
                               // 最多可以打开 NOFILE (16) 个文件
                               
  struct inode *cwd;           // 当前工作目录 (Current Working Directory)
                               // 指向当前目录的 inode
                               // 用于解析相对路径
                               
  char name[16];               // 进程名称（用于调试）
                               // 通常是可执行文件的名字
                               // 在 ps 命令或 procdump() 中显示
};
