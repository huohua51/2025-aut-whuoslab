# 实验1：RISC-V引导与裸机启动

**实验目标**
**通过参考xv6的启动机制，理解并实现最小操作系统的引导过程，最终在QEMU中输**
**出"Hello OS"。**

#### `entry.S`

> 作用：CPU 上电（QEMU `-bios none -kernel`）后在 **M 模式**进入 `_entry`；在这里**建立每核私有栈**、完成最早期的最小运行时准备，然后**跳到 C 的 `start()`**。

```asm
# qemu -kernel loads the kernel at 0x80000000
# and causes each hart (i.e. CPU) to jump there.
# kernel.ld causes the following code to
# be placed at 0x80000000.
.section .text
.global _entry
_entry:
```

- `-kernel`：QEMU 把 ELF 装入内存，并把 **PC=ELF.e_entry**（由链接脚本 `ENTRY(_entry)` 指定）。
- `.section .text`：下面的指令进入代码段。
- `.global _entry`：把 `_entry` 符号导出到全局符号表，供 **链接器设入口**、供调试器识别。

------

```asm
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
```

- **为什么第一件事是设置栈**：C 函数进入/返回需要用栈保存 ra/寄存器溢出参数等；如果没栈直接调用 C，会把数据压到**未知地址**，瞬间崩溃。
- `la sp, stack0`：把 **每核栈数组的起始地址**（低地址）装入 `sp`。注意这只是基址，不是每核的“栈顶”。

```asm
        li a0, 1024*4
```

- `a0=4096`（每核 4KB 启动栈；xv6 里简单够用：只跑几层初始化调用栈）。

```asm
        csrr a1, mhartid
```

- 读 **CSR** `mhartid`（硬件线程/核的编号）。`0,1,2,...` 依核不同。
- **Zicsr** 扩展必须启用（工具链 `-march=rv64imac_zicsr`），否则汇编器会报“需要 zicsr”。

```asm
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
```

- 计算 `sp = stack0 + (hartid + 1) * 4096`。

  - **为什么加 1**：想让 **hart0 的 sp** 指向 **第 0 个 4KB 栈块的高端**（“块末端”，栈向下生长），而不是块起始地址。

  - 形象化（地址从低到高）：

    ```
    stack0
    ├─ [hart0 栈 4KB] ──┐ ← sp 指向这里（块末端）
    ├─ [hart1 栈 4KB] ──┐
    ├─ [hart2 栈 4KB] ──┐
    ```

  - 这段写法暗含：**每核共享同一个大数组，但各取自己的 4KB 段**作为私有启动栈。

------

```asm
        # jump to start() in start.c
        call start
```

- 依 RISC-V ABI，`call` = `auipc + jalr`，把返回地址写到 **ra(x1)**。
- 进入 C 函数 `start()` 前，**栈已经就绪**，可以安全执行 C 代码。
- 在 xv6 的路径中，`start()` 最终会 `mret` 到 `main()`（S 模式），因此**不会返回**；若错误返回，也有兜底：

```asm
spin:
        j spin
```

- 兜底自旋，避免落入未知区域。调试器（GDB）附着也方便。

------

####  start.c

> 作用：仍在 **M 模式**。配置好 CSR，**把“返回点”设置到 S 模式的 `main()`**，然后执行 `mret` 实现 **M→S** 跳转；同时准备 S 模式可用的中断/计时器/PMP 等。

```c
#include "types.h"        // xv6 的基础类型定义（uint64 等）
#include "param.h"        // NCPU 等常量
#include "memlayout.h"    // 内存布局常量（KERNBASE 等）
#include "riscv.h"        // 内联函数：r_mstatus/w_mstatus 等，位宏 MSTATUS_MPP_S 等
#include "defs.h"         // 函数原型（main、consoleintr 等）
```

- 这些头文件把 CSR 读写封装成 `static inline`，避免手写 `csrr/csrw`。

```c
void main();
void timerinit();

// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];
```

- **与 entry.S 的契约**：这里真正**定义**了 `stack0`（全局数组，16 字节对齐，大小 4KB×NCPU），使得汇编里的 `la sp, stack0` 有了实体。
- 对齐到 16：避免 `sp` 违背 ABI 要求（调用前 sp 16-byte aligned）。

```c
// entry.S jumps here in machine mode on stack0.
void start()
{
  // set M Previous Privilege mode to Supervisor, for mret.
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);
```

- `mstatus.MPP`（bits 12:11）= **M Previous Privilege**。
  - `MPP=01` = **S 模式**。
  - 这一步决定了**执行 `mret` 时会降到 S 模式**。
- 做法：先清空 `MPP` 位域，再置位为 `S`。

```c
  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  w_mepc((uint64)main);
```

- 设置 **mepc**（Machine Exception PC）为 `main()` 的地址。
- 执行 `mret` 时，`pc ← mepc`，且特权级切换到 `MPP` 指示的模式（此处为 S）。
- **为什么需要 `-mcmodel=medany`**：
  - RISC-V 高地址（如 `0x80..`）的符号装载需要 **PC 相对 + 重定位序列**；
  - 若代码模型不允许从任意 PC 装载任意符号地址，会出现 **`R_RISCV_HI20` 截断**的链接错误。

```c
  // disable paging for now.
  w_satp(0);
```

- `satp=0`：**关闭分页**（裸跑物理地址）。
- xv6 后续在 S 模式里会建页表、开启分页；此处为早期启动不涉及页表。

```c
  // delegate all interrupts and exceptions to supervisor mode.
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);
```

- **medeleg/mideleg**：把**异常/中断**从 M 模式**委托**到 S 模式处理（否则全部 trap 都进 M）。
  - `0xffff` 是教学演示的“全开”写法：把低 16 个异常/中断源委托给 S。
  - 更严谨的做法应逐位控制（哪些必须留在 M）。
- `sie |= SEIE | STIE`：**开 S 模式**的外部中断（SEIE）与计时器中断（STIE）。注意这只是设置 **SIE CSR**，真正能打断还要后续使能 `mie/mstatus` 相关位与具体设备。

```c
  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);
```

- **PMP**（物理内存保护）默认可能把 S 模式限制住。
- 这里把 `pmpaddr0` 设成“**覆盖全部物理地址**”的最大值（NAPOT/NA 相关编码），并把 `pmpcfg0=0xf`（RWX 全权限 + A=NAPOT），使 **S 模式**能访问全部物理内存。
- 没有这步，进入 S 模式可能因访问受限而触发异常。

```c
  // ask for clock interrupts.
  timerinit();
```

- 做 **计时器子系统的最小初始化**，为 S 模式提供时钟中断。

```c
  // keep each CPU's hartid in its tp register, for cpuid().
  int id = r_mhartid();
  w_tp(id);
```

- **tp 寄存器**（x4，线程指针）在 xv6 里被用作 **per-CPU 快捷索引**（如 `cpuid()` 读 `tp` 拿 hartid）。
- 规范上 tp 常用于 TLS；教学内核拿来当桶，方便。

```c
  // switch to supervisor mode and jump to main().
  asm volatile("mret");
}
```

- **关键一步**：
  - `pc ← mepc`（跳到 `main()`）
  - **特权级**：`M → MPP`（此处 = `S`）
  - 同时恢复 M 模式的中断使能保存位等
- 从这行开始，**CPU 在 S 模式执行 `main()`**；trap 将按照前面的委托规则进入 S 模式处理。

------

### 计时器初始化 `timerinit()`（逐行 + sstc 解释）

```c
void timerinit()
{
  // enable supervisor-mode timer interrupts.
  w_mie(r_mie() | MIE_STIE);
```

- `mie.STIE`（S 模式计时器中断允许）设 1。
- 注意：这是 **M 模式下对 MIE 的写**，xv6 的内联封装做了兼容；后续在 S 态也会控制 `sie`/`sstatus`。

```c
  // enable the sstc extension (i.e. stimecmp).
  w_menvcfg(r_menvcfg() | (1L << 63)); 
```

- **Sstc 扩展**：允许 S 模式访问 `stimecmp` 计时器比较寄存器（无需 M 模式代理）。
- `menvcfg[63]`（即 STCE）置位打开此功能。

```c
  // allow supervisor to use stimecmp and time.
  w_mcounteren(r_mcounteren() | 2);
```

- **mcounteren** 控制 S/U 能否访问某些计数器（time、instret、hpmcounterX）。
- 置位 bit1（值 `2`）允许 S 模式访问 **`time`**（时间基计数器），与 sstc 一起使用。

```c
  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
}
```

- **预约一次** S 模式计时器中断：`stimecmp = time + Δ`（这里 Δ≈1000000 tick）。
- 当 `time >= stimecmp`，硬件置位 S 模式计时器中断 pending；若已启用 SIE，将触发进 S 模式的 trap，驱动时钟 tick。

> 注意：要真正让中断“打进来”，还需要在合适时机打开 **sstatus.SIE**（S 全局中断使能）并保证外设/时基有效。xv6 会在后续路径里完成这些细节。

------

####  `kernel.ld`

> 作用：把各输入段（`.text/.rodata/.data/.bss/...`）**按指定物理地址**从 `0x80000000` 起**排布**，并导出关键边界符号（如 `etext/end`）。此外，保证 **trampoline 段**恰好 1 页（方便页表映射/保护）。

```ld
OUTPUT_ARCH( "riscv" )
ENTRY( _entry )
```

- 目标架构标记为 `"riscv"`（方便工具识别）。
- 入口符号 `_entry` → 写入 ELF 头的 `e_entry` → QEMU 跳入。

```ld
SECTIONS
{
  /*
   * ensure that entry.S / _entry is at 0x80000000,
   * where qemu's -kernel jumps.
   */
  . = 0x80000000;
```

- **位置计数器 `.`**：把“接下来要放的内容”从 `0x80000000` 开始。
- QEMU virt 机型常用这个起始物理地址作为内核基址。

```ld
  .text : {
    kernel/entry.o(_entry)
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    PROVIDE(etext = .);
  }
```

- **把 `_entry` 放在 .text 的最前**：
  - `kernel/entry.o(_entry)` 这一行强制 `_entry` 先布局，确保入口地址就是 `0x80000000`。
- 收集所有 `.text*` 代码到此段。
- `ALIGN(0x1000)`：页对齐，为接下来的 trampoline 单页留出干净的页起点。
- `_trampoline = .;  *(trampsec)`：把 **trampoline 段**（用户态↔内核态切换的“跳板”代码）**单独收集到一页**。
- 再 `ALIGN(0x1000)` 并用 `ASSERT` 断言 **`. - _trampoline == 0x1000`**：确保 trampoline **恰好 1 页**（不大不小）。这样在建页表时可以“单独映射/只映射这一页”，安全且清晰。
- `PROVIDE(etext = .)`：导出 **etext**（代码+trampoline 的末地址）。

```ld
  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }
```

- **只读数据段**。`srodata` = small rodata（GCC 小数据优化），合起来放。
- 16 字节对齐，满足通用 ABI/性能需求。

```ld
  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }
```

- **已初始化可写全局/静态**。
- `sdata` = small data，和 `.data` 汇总管理。

```ld
  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  }

  PROVIDE(end = .);
}
```

- **未初始化全局/静态**在 `.bss`；`sbss` 是其“小数据”变体。
- 这里的脚本没写 `NOLOAD`，但实际 ELF 程序头常把 `.bss` 标成 NOBITS，不占文件体积、只占运行地址空间。
- 导出 **`end`**：镜像末尾标记（常用于早期**内存分配器起点**、或调试打印边界）。

> 简化版本里（实验 1）把 `__bss_start/__bss_end` 也 `PROVIDE()` 出来，便于汇编清 BSS，这和 xv6 的思路一致，只是 xv6 版本在其他文件里导出了这些符号。

## 0.环境与目录

- 工具链：`riscv64-unknown-elf-gcc`（带 `zicsr`）、QEMU 7.0+
- 运行命令：`qemu-system-riscv64 -machine virt -nographic -bios none -kernel build/kernel.elf`
- 目录结构（git 管理）：

```
riscv-os/
├── kernel/
│   ├── entry.S       # 启动汇编：设栈、清 BSS、跳 C
│   ├── start.c       # C 入口：UART 打印 Hello OS
│   └── linker.ld     # 链接脚本：段布局与符号
├── Makefile          # 构建脚本：all/run/qemu/clean
├── README.md         # 编译运行说明
└── report.md         # 实验报告/笔记（本文）
```

## 1. RISC-V 启动路径对照

### 1.1 正常 OS（xv6/Linux）启动概览

1. 上电 → CPU 进入 **M 模式**（最高特权级）。
2. **固件**（SBI/OpenSBI）初始化硬件，设置 `mstatus.MPP`、`mepc`，`mret` 切到 **S 模式**。
3. **S 模式**内核启动，建立中断/内存管理；随后运行 **U 模式**进程。
4. 进程 `exit()`，内核回收或关机。

> 关键：通过 `mret` 从 **M→S**，再由 **S** 管理 **U** 进程。

### 1.2 本实验最小 OS 启动概览

- QEMU `-bios none -kernel` 直接把控制权交给 ELF 入口 `_entry`（`0x80000000`）。
- `entry.S`：**设置栈** → **（hart0）清 BSS** → **跳到 `start()`**。
- `start.c`：**轮询 UART 寄存器**发送字符串 → **死循环驻留**。
- 不切到 S 模式，不涉及时钟/中断/分页（后续实验再加）。

------

## 2. 启动代码与内存布局

### 2.1 `kernel/entry.S`（关键职责）

```asm
.section .text
.globl _entry
_entry:
    li      t0, 0x10000000       # 可做串口打点

    la      sp, stack_top        # ★ 设置栈指针（栈向下生长）

    # ★ 仅 hart0 清 BSS，避免多核并发擦写
    csrr    t2, mhartid
    bnez    t2, 1f
    la      t3, __bss_start
    la      t4, __bss_end
0:  bgeu    t3, t4, 1f
    sd      x0, 0(t3)
    addi    t3, t3, 8
    j       0b
1:
    la      t0, start            # 跳到 C 入口
    jr      t0

# 独立的栈段放到 .bss.stack，避免被 BSS 清零误伤
.section .bss.stack,"aw",@nobits
.align 16
.globl  stack_top
.equ    STACK_SIZE, 16*1024
stack_area:
    .skip   STACK_SIZE
stack_top:
```

**为什么只让 hart0 清 BSS？**
 避免 N 个核同时擦同一片内存产生竞态/浪费。此处写法对“只起 1 核”与“未来开多核”都兼容。

------

### 2.2 `kernel/linker.ld`（关键职责）

```ld
OUTPUT_ARCH("riscv")
ENTRY(_entry)

SECTIONS {
  . = 0x80000000;                    /* ★ 程序起始物理地址 */

  .text : {
    *(.text .text.*)
    *(.rodata .rodata.*)
  }
  . = ALIGN(0x1000);
  PROVIDE(etext = .);                /* ★ 代码末尾标记 */

  .data : { *(.data .data.*) }
  . = ALIGN(0x1000);
  PROVIDE(edata = .);                /* 可用作 bss 起点 */

  .bss (NOLOAD) : {
    __bss_start = .;                 /* ★ BSS 起点符号 */
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    __bss_end = .;                   /* ★ BSS 终点符号 */
  }
  . = ALIGN(0x1000);
  PROVIDE(end = .);                  /* ★ 镜像末尾标记 */

  .bss.stack (NOLOAD) : {
    *(.bss.stack)
  }
}
```

- **位置计数器 `.`**：从 `0x80000000` 开始，按**书写顺序**依次摆 `.text → .rodata → .data → .bss → .bss.stack`。
- **NOLOAD 段**：`.bss` 与 `.bss.stack` 运行时存在，镜像文件不占体积。
- **导出符号**：`__bss_start/__bss_end/etext/edata/end` 链接期固化为地址，供汇编/C 使用（不是运行时“查表”）。

> **验证布局**
>
> ```
> riscv64-unknown-elf-objdump -h build/kernel.elf
> riscv64-unknown-elf-nm -n build/kernel.elf | egrep '(_entry|start|__bss_|etext|edata|end|stack_top)'
> ```

------

### 2.3 `kernel/start.c`（关键职责）

```c
#include <stdint.h>
static volatile unsigned char * const UART0 = (unsigned char*)0x10000000;

static inline void uart_putc(char c){
  volatile unsigned char *lsr = UART0 + 5;        // LSR @ +5
  while(((*lsr) & (1<<5)) == 0) ;                 // ★ 等待 THRE=1，可写
  *UART0 = (unsigned char)c;                      // THR @ +0
}
static inline void uart_puts(const char* s){
  while(*s){
    if(*s == '\n') uart_putc('\r');               // 转 CRLF
    uart_putc(*s++);
  }
}
void start(void){
  uart_puts("Hello OS\n");
  for(;;);                                        // ★ 裸机无退出路径，驻留
}
```

- **THR/LSR 的关系**：`THR` 是“送字节”的口子；`LSR.bit5(THRE)` 表示可再送。
- **为什么要等 THRE**：否则覆盖未发送完的字节 → 丢字/乱码。
- **为什么死循环**：裸机没有 CRT/OS 来承接“返回”，返回会跑飞；驻留或 `wfi` 更稳。

------

## 3. Makefile 与运行

- 关键编译参数：

  - `-mcmodel=medany`：允许在高地址（如 `0x80000000`）访问常量，修复 `R_RISCV_HI20` 截断。
  - `-march=rv64imac_zicsr`：使能 `csrr/csrw` 等 CSR 指令。
  - `-nostdlib -nostartfiles -ffreestanding`：裸机场景不依赖 C 运行时。

- 运行：

  ```
  make
  make run
  ```

  `-nographic` 将串口映射到你的终端，看到输出：

  ```
  Hello OS
  ```

- （可选多核）多核需 `-smp N`；当前代码会让每核都跳 `start()`，但**只有一份栈不安全**。如果打算单核，请 **park 非 0 号 hart**。



# 任务 1：理解 xv6 启动流程

## 1) 阅读 `kernel/entry.S`

### Q1：为什么第一条指令是**设置栈指针**？

- **原因**：进入 C 代码前必须有合法栈；函数序言会压栈保存返回地址/保存被调用者寄存器/开局分配栈帧。如果 `sp` 未初始化，会往未知内存写数据，立刻崩溃。
- **xv6**：先算出每核 4KB 启动栈的地址，再 `sp = stack0 + (hartid+1)*4096`。
- **你的最小 OS**：单核路径直接 `la sp, stack_top` 即可（更简单）。

### Q2：`la sp, stack0` 中的 **`stack0`** 在哪里定义？

- **xv6**：在 `start.c` 里定义
   `__attribute__((aligned(16))) char stack0[4096 * NCPU];`
- **你的最小 OS**：不用数组，而是在 `entry.S` 里用专用段 `.bss.stack` 预留了一块 16KB，导出符号 `stack_top`；`la sp, stack_top` 直接取这块栈顶。两种做法都正确，本质上都是“给 C 提供一块能用的栈”。

### Q3：为什么要**清零 BSS 段**？

- **C 语义要求**：未初始化的全局/静态变量在程序开始时都必须为 **0**。
- **裸机没有 CRT**（C 运行时），不会自动清 BSS，必须由你在早期启动阶段手动清零（通常只让 hart0 清，避免多核并发擦同一片内存）。

### Q4：如何从汇编**跳转到 C 函数**？

- 典型做法是：
  - `la t0, start` + `jr t0`，或
  - 直接 `call start`（jal 伪指令展开）。
- **前提**：已经设置好 `sp`（否则进入 C 立即炸）。

------

## 2) 分析 `kernel/kernel.ld`（链接脚本）

### Q1：`ENTRY(_entry)` 的作用是什么？

- 把 ELF 头的入口 `e_entry` 设为 `_entry` 符号地址。
- QEMU `-bios none -kernel kernel.elf` 启动时，会把 **PC** 设为 `e_entry`，于是 CPU 从 `_entry` 开始执行。

### Q2：为什么代码段要放在 `0x80000000`？

- 这是 QEMU `virt` 机型上通用的**内核装载物理地址**；很多教学内核（xv6、你的最小内核）都从这里起步，简化 bring-up。
- 在链接脚本里 `.` 设为 `0x80000000`，就把输出镜像的第一段（.text）定位在这里。

### Q3：`etext`、`edata`、`end` 符号有什么用途？

- `etext`：**代码/只读数据**末尾地址（常用于确定文本末、统计等）。
- `edata`：**已初始化数据 (.data)** 末尾地址（常作 bss 起点或边界校验）。
- `end`：镜像整体末尾（常作为“早期堆/空闲内存起点”的参考；或简单调试/打印边界）。
- **你的脚本**还导出了 `__bss_start/__bss_end`，供 BSS 清零使用。

------

## 3) RISC-V 启动机制要点

- **特权级规范 §3.1**：上电后进入 **M 模式**，PC 指向复位入口（QEMU `-bios none` + `-kernel` 情况下就是 ELF 入口 `_entry`）。
- xv6 在 `start()` 里：
  - 设置 `mstatus.MPP = S`，`mepc = main`，`mret` → 切到 **S 模式**执行 `main()`。
  - 委托 `medeleg/mideleg`、配置 `sie/mie/sstatus`、PMP 全开、定时器初始化（`stimecmp` + sstc + `mcounteren`）。
- **我的最小 OS**：不切 S 模式，留在 **M 模式**直接打印并死循环，更简洁。

------

# 任务 2：设计最小启动流程

## 1) 启动流程图

```
QEMU (-bios none) → PC = 0x80000000
         |
         v
_entry (entry.S)
  ├─ 设栈 sp = stack_top
  ├─ (仅 hart0) 清 BSS: [__bss_start, __bss_end)
  └─ 跳转 C 入口 start()
         |
         v
start() (start.c)
  ├─ (可选) uartinit()           # QEMU 下可省
  ├─ uart_puts("Hello OS\n")
  └─ for(;;)                     # 裸机驻留，无退出路径
```

## 2) 内存布局方案（ `linker.ld`）

- 起始地址：`0x80000000`
- 段顺序（自上而下）：`.text → .rodata → .data → .bss → .bss.stack`
- 导出符号：`etext / edata / end / __bss_start / __bss_end / stack_top`

## 3) 必需的硬件初始化

- **栈**： `_entry` 里设置 `sp`。
- **BSS 清零**：只让 hart0 清 `[__bss_start, __bss_end)`。
- **UART**：在 QEMU virt 上，直接“轮询 LSR.bit5（THRE）→ 写 THR”即可发字；严格做法还需设置 LCR/FCR/IER，但 QEMU 容忍省略。

------

## 关键问题回答

### Q1：栈放在内存的哪个位置？需要多大？

- 位置：单核可在 `.bss.stack` 段预留一块（你的做法），符号 `stack_top` 指向顶端。
- 大小：最小实验 **16KB** 足够；若以后有深栈调用/中断嵌套/调试器回溯，酌情加大。
- 多核时应按 `mhartid` 为各核分离栈（xv6 做法）。

### Q2：是否需要清零 BSS 段？为什么？

- **需要**。C 语义：未初始化全局/静态必须为 **0**。
- 不清零会出现未定义值，通常“偶尔正常、偶发异常”的**隐蔽 bug**。
- 多核只让 hart0 清，避免并发擦写。

### Q3：最简串口输出要配置哪些寄存器？

- **最小只用两个（QEMU 容忍）**：
  - `LSR @ base+5`（读）：检查 bit5=THRE（发送保持寄存器空）
  - `THR @ base+0`（写）：写要发的字节
- 严格初始化（更通用）：
  - `LCR=0x80` 进入设波特率模式 → `DLL/DLM` 设 Baud → `LCR=0x03`（8N1）
  - `FCR=0x07` 重置并开 FIFO
  - 需要中断时 `IER` 置位

------

# 任务 3：实现启动汇编代码

-  创建 `kernel/entry.S`
-  入口 `_entry` 与 `ENTRY(_entry)` 一致
-  设栈（单核：`la sp, stack_top`；多核：按 `mhartid` 切片）
-  （可选但推荐）仅 hart0 清 BSS：遍历 `[__bss_start, __bss_end)`
-  跳转 `start()`（`tail start`/`la+jr`/`call` 均可）

**调试检查点（可选）**

```asm
li t0, 0x10000000      # UART base
li t1, 'S'             # “进入 _entry”标记
sb t1, 0(t0)

la sp, stack_top
li t1, 'P'             # “栈就绪”标记
sb t1, 0(t0)
```

你已经验证过：在 QEMU 终端可看到 `SPC Hello OS` 这类打点串（S/P/C）。

------

# 任务 4：编写链接脚本

-  `OUTPUT_ARCH("riscv")`（目标架构标记）
-  `ENTRY(_entry)`（入口符号）
-  `.` 从 `0x80000000` 开始（QEMU virt 习惯地址）
-  `.text/.rodata/.data/.bss` 顺序组织；`.bss` 用 `NOLOAD`（可选）
-  导出 `__bss_start/__bss_end/etext/edata/end`
-  独立 `.bss.stack (NOLOAD)` 放栈，避免被 BSS 清零误伤

**验证命令**

```bash
riscv64-unknown-elf-objdump -h build/kernel.elf
riscv64-unknown-elf-nm -n build/kernel.elf | egrep '(_entry|start|__bss_|etext|edata|end|stack_top)'
```

------

# 任务 5：实现串口驱动（最小功能）

### 寄存器

- **THR**（Transmit Holding Register）：`UART0 + 0`（写）
- **LSR**（Line Status Register）：`UART0 + 5`（读），bit5=THRE（1=可写）

### 发送一个字符的流程

1. 轮询：`while ((LSR & (1<<5)) == 0) ;`
2. 写：`THR = ch;`

### 为什么检查 THRE？

- 防止覆盖上一次尚未被移入移位寄存器的字节 → **丢字/乱码**。
- 这是 16550 经典的“忙等”模型，简单可靠。

### 实现策略

- 先实现 `uart_putc()`，再封装 `uart_puts()`（处理 `\n`→`\r\n`）。

------

# 任务 6：完成 C 主函数

### 设计要点

- 函数名可以不是 `main`，但要与汇编跳转目标一致（你的是 `start`）。
- 裸机**无退出路径**，不要 `return`；用 `for(;;)` 驻留或 `wfi`。
- 为防止意外返回，可在 `_entry` 末尾自旋（xv6 的 `spin:`）。

### 编译后检查内存布局（命令）

```bash
riscv64-unknown-elf-objdump -h build/kernel.elf
riscv64-unknown-elf-nm build/kernel.elf | grep -E "(start|end|text|__bss_)"
```

------

# 调试策略

1. **硬件验证阶段**：在 `_entry` 里直接 `sb 'S'` 到 `0x10000000` 看是否有输出。
2. **启动验证阶段**：设置好栈后再 `sb 'P'`，确认走过关键点；跳到 `start()` 后再 `sb 'C'`。
3. **功能验证阶段**：`uart_puts("Hello OS\n")` 全串成功输出。

------

# GDB 调试技巧

```bash
# 一终端：带 GDB 端口启动
qemu-system-riscv64 -machine virt -nographic -bios none -kernel build/kernel.elf -S -s

# 另一终端：连上调试
riscv64-unknown-elf-gdb build/kernel.elf
(gdb) target remote :1234
(gdb) b _entry
(gdb) c
(gdb) layout asm
(gdb) si     # 单步汇编
```

------

# 思考题

1. **启动栈的设计**
   - **大小**：基于最大调用深度/是否启中断/是否带调试器回溯/是否递归；裸机最小 16KB 起步。
   - **太小**：栈溢出会覆写 .bss/.data 或返回地址，表现为随机崩溃；可在栈底放“哨兵”检测、或用 PMP 保护页、或在调试中观察 `sp` 最小值。
2. **BSS 段清零**
   - 若不清零，未初始化全局/静态变量读出**随机垃圾值**；很多 “偶发 bug” 就来自这里。
   - 可以省略的唯一合理场景：项目明确承诺**绝不使用**未初始化全局/静态，且清楚所有第三方库也不使用（几乎不现实）。教学时建议**总是清**。
3. **与 xv6 的对比**
   - **你的简化**：不切 S 模式、不建页表、无 trap/中断框架、UART 轮询而非中断、单核或未做 per-CPU 栈。
   - **潜在问题**：一旦要加进程/时钟/系统调用，就必须补 `mstatus/mepc/mret`、委托、PMP、`stimecmp`、页表、trampoline、trap 向量等。
4. **错误处理**
   - UART 初始化失败（在真机或严格固件上可能发生）：
     - 回退到“忙等 + 打点” 或 用 `ebreak` 停住给调试器；
     - 尝试按 16550 手册完整配置 LCR/DLL/DLM/FCR/IER；
     - 打印错误码/寄存器状态（若能打印）。
   - 最小错误显示机制：约定几个不同的单字符闪烁或重复打印（如 `E1/E2`），或在不同地址打点区间连续输出，便于定位停在哪一步。

# 深入思考

- **多核简化**：
  - 只启 **hart0**，其他核 `wfi`/自旋；或者按 `mhartid` 做 **per-CPU 栈**，并且只让 hart0 清 BSS、做一次性全局初始化，其它核等待 barrier。
- **最小内存管理**：
  - 物理内存分配器（从 `end` 往后发放页），页表/分页可先不做；
  - 若要跑用户态或保护内核，必须建页表（sv39）、trampoline、切 S/U、trap 返回。

