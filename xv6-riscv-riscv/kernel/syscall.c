
// kernel/syscall.c - 系统调用分发和参数提取模块

//
// 主要功能：
// 1. 参数提取：从用户态寄存器中提取系统调用参数
// 2. 类型转换：提供类型安全的参数提取接口（int/long/addr/string）
// 3. 地址验证：通过 copyin/copyout 验证用户空间地址的合法性
// 4. 系统调用分发：根据系统调用号调用相应的处理函数
// 5. errno 机制：统一处理错误码，符合 POSIX 标准
//
// 设计原则：
// - 类型安全：为不同类型参数提供专门的提取函数
// - 内存安全：所有用户地址必须经过验证
// - 错误处理：统一的 errno 机制，便于用户诊断错误
// - 简洁接口：系统调用实现者只需关注业务逻辑
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"
#include "errno.h"


// fetchaddr - 从用户空间读取一个 uint64 值

//
// 功能：从当前进程的用户地址空间读取 8 字节数据
//
// 参数：
//   addr: 用户空间的虚拟地址
//   ip:   内核空间的缓冲区，用于存储读取的值
//
// 返回值：
//   0:  成功
//   -1: 失败（地址非法或越界）
//
// 安全检查：
//   1. 地址必须在进程地址空间内（< p->sz）
//   2. 地址 + 8 字节也必须在地址空间内（防止整数溢出）
//   3. copyin 会进一步检查页表映射和权限
//
// 使用场景：
//   - 读取用户传入的指针指向的值
//   - 例如：wait(&status) 中读取 status 变量的地址
//
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  
  // 安全检查：地址必须在进程地址空间内
  // 两个检查都需要，防止 addr + sizeof(uint64) 整数溢出
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz)
    return -1;
  
  // 通过页表从用户空间复制数据到内核空间
  // copyin 会检查：
  //   - 页面是否映射
  //   - 是否有 PTE_U 标志（用户可访问）
  //   - 是否触发 COW（如果是写操作）
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  
  return 0;
}


// fetchstr - 从用户空间读取以空字符结尾的字符串

//
// 功能：从用户地址空间复制字符串到内核缓冲区
//
// 参数：
//   addr: 用户空间字符串的起始地址
//   buf:  内核空间缓冲区
//   max:  缓冲区最大长度（包括终止符）
//
// 返回值：
//   >= 0: 字符串长度（不包括 '\0'）
//   -1:   失败（地址非法、无终止符、或超过最大长度）
//
// 安全特性：
//   - copyinstr 会逐字节检查，遇到 '\0' 或达到 max 时停止
//   - 防止缓冲区溢出
//   - 防止读取内核地址
//
// 使用场景：
//   - 读取文件路径：open("/path/to/file", ...)
//   - 读取程序名称：exec("/bin/sh", ...)
//
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  
  // copyinstr 会：
  //   1. 逐字节从用户空间复制
  //   2. 遇到 '\0' 时停止
  //   3. 达到 max 字节时停止
  //   4. 检查每个页面的权限
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  
  // 返回字符串长度（不包括 '\0'）
  return strlen(buf);
}


// argraw - 从 trapframe 中提取原始参数值

//
// 功能：根据参数编号从对应的寄存器中读取值
//
// 参数编号与 RISC-V 寄存器的对应关系：
//   n=0 → a0 (x10)  函数参数 0 / 返回值
//   n=1 → a1 (x11)  函数参数 1
//   n=2 → a2 (x12)  函数参数 2
//   n=3 → a3 (x13)  函数参数 3
//   n=4 → a4 (x14)  函数参数 4
//   n=5 → a5 (x15)  函数参数 5
//
// 注意：
//   - 这是内部函数，不直接暴露给系统调用实现
//   - 不进行类型转换和安全检查
//   - 超过 5 个参数的系统调用需要通过栈传递（xv6 未实现）
//
// 设计思想：
//   - 提供统一的参数访问接口
//   - 隐藏 trapframe 的内部结构
//   - 便于移植到其他架构
//
static uint64
argraw(int n)
{
  struct proc *p = myproc();
  
  // 根据参数编号返回对应寄存器的值
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  
  // 不应该到达这里（参数编号越界）
  panic("argraw");
  return -1;
}


// argint - 提取第 n 个系统调用参数（32位整数）

//
// 功能：从 trapframe 中提取整数参数
//
// 参数：
//   n:  参数编号（0-5）
//   ip: 指向内核整数变量的指针，用于存储提取的值
//
// 使用示例：
//   uint64 sys_kill(void) {
//       int pid;
//       argint(0, &pid);  // 提取第 0 个参数（PID）
//       return kkill(pid);
//   }
//
// 注意：
//   - 虽然寄存器是 64 位，但这里只取低 32 位
//   - 适用于 int 类型参数
//   - 有符号整数
//
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}


// arglong - 提取第 n 个系统调用参数（64位有符号整数）

//
// 功能：从 trapframe 中提取 64 位有符号整数参数
//
// 参数：
//   n:  参数编号（0-5）
//   ip: 指向内核 long 变量的指针
//
// 使用场景：
//   - 需要 64 位精度的数值（如大文件偏移量）
//   - 有符号的大整数
//
// 与 argint 的区别：
//   - argint:  返回 int (32 位有符号)
//   - arglong: 返回 long (64 位有符号)
//   - argaddr: 返回 uint64 (64 位无符号，用于地址)
//
void
arglong(int n, long *ip)
{
  *ip = argraw(n);
}


// argaddr - 提取第 n 个系统调用参数（指针/地址）

//
// 功能：从 trapframe 中提取地址参数
//
// 参数：
//   n:  参数编号（0-5）
//   ip: 指向内核 uint64 变量的指针，用于存储地址
//
// 重要说明：
//   - 只提取地址值，不验证地址的合法性！
//   - 合法性检查由 copyin/copyout/copyinstr 完成
//   - 这样设计避免重复检查，提高效率
//
// 使用示例：
//   uint64 sys_read(void) {
//       uint64 buf_addr;
//       argaddr(1, &buf_addr);  // 提取缓冲区地址
//       // copyin 会检查 buf_addr 的合法性
//       copyin(p->pagetable, dst, buf_addr, len);
//   }
//
// 为什么不在这里检查？
//   - copyin/copyout 已经有完整的检查逻辑
//   - 避免重复代码
//   - 某些地址可能是可选的（如 wait(0) 中的 NULL）
//
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}


// argstr - 提取第 n 个系统调用参数（字符串）

//
// 功能：从用户空间提取字符串参数到内核缓冲区
//
// 参数：
//   n:   参数编号（0-5）
//   buf: 内核缓冲区，用于存储字符串
//   max: 缓冲区最大长度（包括终止符）
//
// 返回值：
//   >= 0: 字符串长度（不包括 '\0'）
//   -1:   失败（地址非法、无终止符、或超过最大长度）
//
// 工作流程：
//   1. 从 trapframe 提取字符串地址（argaddr）
//   2. 从用户空间复制字符串（fetchstr）
//   3. fetchstr 内部调用 copyinstr 进行安全检查
//
// 使用示例：
//   uint64 sys_open(void) {
//       char path[MAXPATH];
//       if(argstr(0, path, MAXPATH) < 0)
//           return -1;  // 文件路径提取失败
//       // 现在 path 包含用户传入的文件路径
//   }
//
// 安全特性：
//   - 防止缓冲区溢出（max 限制）
//   - 检查用户地址合法性
//   - 确保字符串有终止符
//
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);           // 提取字符串地址
  return fetchstr(addr, buf, max);  // 复制字符串到内核
}


// 系统调用处理函数的声明

//
// 这些函数在其他文件中实现：
//   - sysproc.c:  进程相关系统调用（fork, exit, wait, kill 等）
//   - sysfile.c:  文件相关系统调用（open, read, write, close 等）
//
// 命名约定：
//   - sys_xxx(): 系统调用包装函数（提取参数，调用内核函数）
//   - kxxx():    内核实现函数（实际功能）
//   - 用户程序调用 xxx()，最终路由到 sys_xxx()
//

// 进程管理系统调用
extern uint64 sys_fork(void);        // 创建子进程（COW优化）
extern uint64 sys_exit(void);        // 终止当前进程
extern uint64 sys_wait(void);        // 等待子进程退出
extern uint64 sys_getpid(void);      // 获取当前进程 PID
extern uint64 sys_kill(void);        // 杀死指定进程
extern uint64 sys_sbrk(void);        // 调整堆大小（支持Lazy分配）
extern uint64 sys_pause(void);       // 睡眠指定 tick 数
extern uint64 sys_uptime(void);      // 获取系统运行时间

// 文件系统调用
extern uint64 sys_open(void);        // 打开文件
extern uint64 sys_read(void);        // 读取文件
extern uint64 sys_write(void);       // 写入文件
extern uint64 sys_close(void);       // 关闭文件
extern uint64 sys_dup(void);         // 复制文件描述符
extern uint64 sys_pipe(void);        // 创建管道
extern uint64 sys_fstat(void);       // 获取文件状态
extern uint64 sys_exec(void);        // 执行程序

// 文件系统管理
extern uint64 sys_chdir(void);       // 改变工作目录
extern uint64 sys_mknod(void);       // 创建设备节点
extern uint64 sys_unlink(void);      // 删除文件
extern uint64 sys_link(void);        // 创建硬链接
extern uint64 sys_mkdir(void);       // 创建目录
extern uint64 sys_symlink(void);     // 创建符号链接
extern uint64 sys_readlink(void);    // 读取符号链接

// 扩展系统调用
extern uint64 sys_setpriority(void); // 设置进程优先级
extern uint64 sys_getpriority(void); // 获取进程优先级
extern uint64 sys_geterrno(void);    // 获取错误码
extern uint64 sys_set_scheduler(void); // 设置调度器类型


// syscalls - 系统调用分发表

//
// 数据结构：函数指针数组
//   - 索引：系统调用号（定义在 syscall.h 中）
//   - 值：  对应的处理函数指针
//
// 初始化方式：C99 指定初始化器
//   [SYS_fork] sys_fork  等价于  syscalls[SYS_fork] = sys_fork
//   - 优点：索引和值在同一行，清晰直观
//   - 自动处理数组大小
//   - 未指定的元素自动初始化为 0
//
// 使用方式：
//   num = trapframe->a7;        // 从 a7 寄存器获取系统调用号
//   syscalls[num]();            // 调用对应的处理函数
//
// 扩展性：
//   - 新增系统调用只需：
//     1. 在 syscall.h 中定义 SYS_xxx
//     2. 在这里添加一行 [SYS_xxx] sys_xxx
//     3. 实现 sys_xxx() 函数
//
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,          // 1:  创建子进程
[SYS_exit]    sys_exit,          // 2:  退出进程
[SYS_wait]    sys_wait,          // 3:  等待子进程
[SYS_pipe]    sys_pipe,          // 4:  创建管道
[SYS_read]    sys_read,          // 5:  读取
[SYS_kill]    sys_kill,          // 6:  杀死进程
[SYS_exec]    sys_exec,          // 7:  执行程序
[SYS_fstat]   sys_fstat,         // 8:  文件状态
[SYS_chdir]   sys_chdir,         // 9:  改变目录
[SYS_dup]     sys_dup,           // 10: 复制文件描述符
[SYS_getpid]  sys_getpid,        // 11: 获取PID
[SYS_sbrk]    sys_sbrk,          // 12: 调整堆
[SYS_pause]   sys_pause,         // 13: 睡眠
[SYS_uptime]  sys_uptime,        // 14: 系统运行时间
[SYS_open]    sys_open,          // 15: 打开文件
[SYS_write]   sys_write,         // 16: 写入
[SYS_mknod]   sys_mknod,         // 17: 创建节点
[SYS_unlink]  sys_unlink,        // 18: 删除文件
[SYS_link]    sys_link,          // 19: 创建链接
[SYS_mkdir]   sys_mkdir,         // 20: 创建目录
[SYS_close]   sys_close,         // 21: 关闭文件
[SYS_setpriority] sys_setpriority,  // 22: 设置优先级
[SYS_getpriority] sys_getpriority,  // 23: 获取优先级
[SYS_geterrno] sys_geterrno,     // 24: 获取errno
[SYS_set_scheduler] sys_set_scheduler, // 25: 设置调度器
[SYS_symlink] sys_symlink,       // 26: 创建符号链接
[SYS_readlink] sys_readlink,     // 27: 读取符号链接
};



// syscall - 系统调用分发器（支持 errno 机制）

//
// 功能：这是所有系统调用的统一入口点
//
// 调用路径：
//   用户程序 → ecall → uservec → usertrap → syscall → sys_xxx → kxxx
//
// 工作流程：
//   1. 从 a7 寄存器获取系统调用号
//   2. 验证系统调用号的合法性
//   3. 调用对应的处理函数
//   4. 处理返回值和 errno
//   5. 将结果写回 a0 寄存器
//
// errno 机制（POSIX 风格）：
//   - 系统调用成功：返回 >= 0，errno 清零
//   - 系统调用失败：返回负错误码（如 -EINVAL）
//   - syscall() 转换：errno = -ret, 返回 -1
//
// 示例：
//   内核函数：return -ENOENT;  (文件不存在，错误码 -2)
//            ↓
//   syscall(): p->errno = 2; p->trapframe->a0 = -1;
//            ↓
//   用户程序：ret = -1; errno = geterrno(); // errno = 2
//
// 这种设计的优点：
//   ✓ 符合 POSIX 标准
//   ✓ 用户程序可以获取详细错误信息
//   ✓ 便于调试和错误处理
//   ✓ 内核代码更清晰（返回负错误码即可）
//
// 调用约定：
//   - 输入：a7 = 系统调用号, a0-a5 = 参数
//   - 输出：a0 = 返回值（成功时的实际值，失败时为 -1）
//   - 副作用：p->errno 被设置
//
void
syscall(void)
{
  int num;               // 系统调用号
  uint64 ret;            // 系统调用返回值
  struct proc *p = myproc();

  // 从 trapframe 的 a7 寄存器获取系统调用号
  // a7 在 usys.S 中被设置：li a7, SYS_xxx
  num = p->trapframe->a7;
  
  // 验证系统调用号的合法性：
  //   1. num > 0: 系统调用号从 1 开始
  //   2. num < NELEM(syscalls): 不能超过数组大小
  //   3. syscalls[num] != 0: 该系统调用必须已实现
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    
    // 调用对应的系统调用处理函数
    // 例如：num = SYS_fork → 调用 sys_fork()
    ret = syscalls[num]();
    
    // ======== errno 机制处理 ========
    //
    // 设计思想：
    //   - 系统调用返回负数表示错误码（如 -EINVAL = -22）
    //   - 转换为 errno（正数）和返回值 -1
    //   - 用户程序统一检查返回值 == -1，然后查看 errno
    //
    if((long)ret < 0) {
      // 失败情况：返回值是负的错误码
      
      // 将负错误码转为正数，存入 errno
      // 例如：ret = -EINVAL(-22) → errno = 22
      p->errno = -(long)ret;
      
      // 系统调用统一返回 -1 表示失败
      // 用户程序：if (fork() == -1) { check errno }
      p->trapframe->a0 = -1;
      
    } else {
      // 成功情况：返回值 >= 0
      
      // 清除 errno（表示没有错误）
      p->errno = EOK;  // EOK = 0
      
      // 返回实际值（可能是 PID、文件描述符、字节数等）
      p->trapframe->a0 = ret;
    }
    
  } else {
    // 未知或未实现的系统调用
    
    // 打印错误信息（便于调试）
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    
    // 设置 errno 为 ENOSYS（功能未实现）
    p->errno = ENOSYS;
    
    // 返回 -1
    p->trapframe->a0 = -1;
  }
}


// 系统调用模块总结

//
// 核心设计思想：
//
// 1. 【分层抽象】
//    用户层（简单）→ 系统调用层（参数提取）→ 内核层（实现）
//
// 2. 【类型安全】
//    argint/arglong/argaddr/argstr 提供类型安全的接口
//
// 3. 【内存安全】
//    所有用户地址通过 copyin/copyout 验证
//
// 4. 【错误处理】
//    统一的 errno 机制，符合 POSIX 标准
//
// 5. 【可扩展性】
//    新增系统调用只需修改三处：
//      - syscall.h: 定义编号
//      - syscall.c: 添加到分发表
//      - sysproc.c/sysfile.c: 实现函数
//
// 6. 【性能优化】
//    - 函数指针数组实现 O(1) 分发
//    - 参数提取避免重复访问 trapframe
//    - 地址验证延迟到实际使用时
//
// 使用示例（完整流程）：
//
// 用户程序：
//   int fd = open("/file", O_RDONLY);
//   if(fd < 0) {
//       printf("Error: %d\n", geterrno());
//   }
//
// 用户库（usys.S）：
//   open:
//     li a7, SYS_open
//     ecall
//     ret
//
// 系统调用分发（syscall.c）：
//   syscall() → syscalls[SYS_open]() → sys_open()
//
// 系统调用实现（sysfile.c）：
//   sys_open() {
//     char path[MAXPATH];
//     argstr(0, path, MAXPATH);  // 提取路径
//     argint(1, &omode);         // 提取模式
//     return kopen(path, omode); // 调用内核函数
//   }
//
// 这就是 xv6 系统调用机制的完整实现！
//

