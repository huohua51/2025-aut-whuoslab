
// kernel/scheduler_ext.c - 扩展调度策略实现

//
// 功能：
// - 提供多种高级调度算法
// - 可通过 set_scheduler() 动态切换
// - 与 proc.c 的调度框架配合使用
//
// 支持的调度器：
// 1. Round-Robin (RR): 轮转调度（在 proc.c 中）
// 2. Priority: 优先级调度
// 3. MLFQ: 多级反馈队列
//
// 设计原则：
// - 只负责"选择进程"，不负责"运行进程"
// - 返回 RUNNABLE 进程指针或 NULL
// - 不修改进程状态（由 scheduler 统一处理）
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// 外部变量声明
extern struct proc proc[NPROC];


// 优先级调度器数据结构

struct priority_queue {
  struct proc *processes[NPROC];  // 进程指针数组
  int count;                      // 当前进程数量
  struct spinlock lock;           // 保护队列的锁
};


// 多级反馈队列（MLFQ）数据结构

//
// 算法思想：
// - 多个优先级队列（level 0 最高，level 4 最低）
// - 新进程从最高优先级开始
// - 用完时间片降级到下一级
// - I/O 操作后可能提升优先级
//
// 时间片分配：
// - Level 0: 1 tick（最短，适合交互式）
// - Level 1: 2 ticks
// - Level 2: 4 ticks
// - Level 3: 8 ticks
// - Level 4: 16 ticks（最长，适合 CPU 密集型）
//
#define MAX_PRIORITY_LEVELS 5

struct mlfq {
  struct priority_queue queues[MAX_PRIORITY_LEVELS];  // 5 个优先级队列
  int current_level;                                  // 当前选择的队列级别
  uint64 time_slices[MAX_PRIORITY_LEVELS];            // 每级的时间片长度
};



// 全局调度器状态
static struct mlfq mlfq_sched;        // MLFQ 调度器实例


// scheduler_init - 初始化扩展调度器

//
// 调用时机：在 main() 中，procinit() 之后调用
//
// 初始化内容：
//  MLFQ 的所有队列和时间片
//
void
scheduler_init(void)
{
  // 初始化 MLFQ 的每个优先级队列
  for(int i = 0; i < MAX_PRIORITY_LEVELS; i++) {
    initlock(&mlfq_sched.queues[i].lock, "mlfq_queue");
    mlfq_sched.queues[i].count = 0;
    
    // 时间片呈指数增长：1, 2, 4, 8, 16 ticks
    // 高优先级队列时间片短（响应快，适合交互）
    // 低优先级队列时间片长（吞吐量高，适合 CPU 密集）
    mlfq_sched.time_slices[i] = 1 << i;
  }
  mlfq_sched.current_level = 0;
  
}


// priority_scheduler - 优先级调度策略

//
// 算法：选择优先级最高的 RUNNABLE 进程
//
// 时间复杂度：O(n)，遍历所有进程
//
// 优点：
// - 重要进程优先执行
// - 适合实时系统
//
// 缺点：
// - 低优先级进程可能饥饿
// - 需要合理设置优先级
//
// 改进建议：
// - 添加优先级老化（aging）机制防止饥饿
// - 区分 CPU bound 和 I/O bound 进程
//
struct proc*
priority_scheduler(void)
{
  struct proc *best_proc = 0;  // 当前找到的最佳进程
  int highest_priority = -1;   // 当前最高优先级
  
  // 遍历所有进程，查找优先级最高的
  for(struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    
    if(p->state == RUNNABLE) {
      // 使用进程的真实优先级字段
      // 优先级范围：0-9，数字越大优先级越高
      if(p->priority > highest_priority) {
        highest_priority = p->priority;
        best_proc = p;
      }
    }
    
    release(&p->lock);
  }
  
  return best_proc;  // 返回优先级最高的进程（或 NULL）
}


// mlfq_scheduler - 多级反馈队列调度策略

//
// 算法：多级反馈队列 (Multi-Level Feedback Queue)
//
// 核心思想：
// - 进程根据行为自动调整优先级
// - I/O 密集型进程→高优先级（响应快）
// - CPU 密集型进程→低优先级（不阻塞交互）
//
// 工作流程：
// 1. 新进程从最高优先级队列 (level 0) 开始
// 2. 如果用完时间片，降级到下一级队列
// 3. 如果主动让出 CPU（I/O 操作），保持或提升优先级
// 4. 总是从最高级非空队列选择进程
//
// 队列调度：
// - Level 0 (最高): 时间片 1 tick，响应最快
// - Level 1: 时间片 2 ticks
// - Level 2: 时间片 4 ticks
// - Level 3: 时间片 8 ticks
// - Level 4 (最低): 时间片 16 ticks，吞吐量最高
//
// 优点：
// - 自适应：无需手动设置优先级
// - 公平：长期运行进程不会饥饿
// - 响应好：交互式进程获得高优先级
//
// 缺点：
// - 实现复杂
// - 需要额外的队列管理
//
struct proc*
mlfq_scheduler(void)
{
  struct proc *selected = 0;
  
  // 从最高优先级队列开始搜索（level 0 最高）
  for(int level = 0; level < MAX_PRIORITY_LEVELS; level++) {
    acquire(&mlfq_sched.queues[level].lock);
    
    // 检查此级队列是否有进程
    if(mlfq_sched.queues[level].count > 0) {
      // 选择队列中的第一个进程（FIFO）
      selected = mlfq_sched.queues[level].processes[0];
      mlfq_sched.current_level = level;
      
      release(&mlfq_sched.queues[level].lock);
      break;  // 找到进程，退出循环
    }
    
    release(&mlfq_sched.queues[level].lock);
  }
  
  return selected;  // 返回选中的进程（或 NULL）
}




// 调度器切换便利函数

//
// 这些函数简化了调度器切换操作
// 无需直接调用 set_scheduler() 并传递函数指针
//

// 切换到轮转调度（默认）
void
use_round_robin(void)
{
  set_scheduler(0);  // NULL 表示恢复默认
}

// 切换到优先级调度
void
use_priority_scheduler(void)
{
  set_scheduler(priority_scheduler);
}

// 切换到多级反馈队列
void
use_mlfq_scheduler(void)
{
  set_scheduler(mlfq_scheduler);
}




// 辅助函数：进程优先级管理

//
// 这些函数用于动态调整进程的调度参数
// 需要 struct proc 中添加相应字段才能完整实现
//

// 调整进程优先级（用于优先级调度器）
//
// 参数：
// - p: 目标进程
// - new_priority: 新的优先级值
//   - 范围：0-9（数字越大优先级越高）
//   - 0: 最低优先级
//   - 9: 最高优先级
//
// 使用场景：
// - 用户通过 nice() 系统调用调整
// - 内核根据进程行为自动调整
//
void
adjust_process_priority(struct proc *p, int new_priority)
{
  if(p == 0) return;
  
  acquire(&p->lock);
  // TODO: 需要在 proc.h 中添加 priority 字段
  // p->priority = new_priority;
  (void)new_priority;  // 暂时避免编译警告
  release(&p->lock);
}




// MLFQ 辅助函数

// 将进程添加到 MLFQ 队列
//
// 参数：
// - p: 要添加的进程
// - level: 目标队列级别 (0-4)
//
// 使用场景：
// - 新进程创建时加入 level 0
// - 进程用完时间片后降级
// - 进程主动让出 CPU 后可能提升
//
void
mlfq_add_process(struct proc *p, int level)
{
  if(level < 0 || level >= MAX_PRIORITY_LEVELS) return;
  if(p == 0) return;
  
  acquire(&mlfq_sched.queues[level].lock);
  
  // 检查队列是否已满
  if(mlfq_sched.queues[level].count < NPROC) {
    mlfq_sched.queues[level].processes[mlfq_sched.queues[level].count] = p;
    mlfq_sched.queues[level].count++;
  }
  
  release(&mlfq_sched.queues[level].lock);
}

// 从 MLFQ 队列中移除进程
//
// 参数：
// - p: 要移除的进程
// - level: 队列级别
//
// 使用场景：
// - 进程退出时
// - 进程在队列间移动时
//
void
mlfq_remove_process(struct proc *p, int level)
{
  if(level < 0 || level >= MAX_PRIORITY_LEVELS) return;
  if(p == 0) return;
  
  acquire(&mlfq_sched.queues[level].lock);
  
  // 在队列中查找并移除
  for(int i = 0; i < mlfq_sched.queues[level].count; i++) {
    if(mlfq_sched.queues[level].processes[i] == p) {
      // 找到了，移除（将后续元素前移）
      for(int j = i; j < mlfq_sched.queues[level].count - 1; j++) {
        mlfq_sched.queues[level].processes[j] = 
          mlfq_sched.queues[level].processes[j + 1];
      }
      mlfq_sched.queues[level].count--;
      break;
    }
  }
  
  release(&mlfq_sched.queues[level].lock);
}


// 调度器性能统计（可选）

//
// 用途：
// - 性能分析
// - 调度器对比
// - 参数调优
//
struct scheduler_stats {
  uint64 total_switches;      // 总切换次数
  uint64 idle_time;           // 空闲时间（无进程可运行）
  uint64 run_time;            // 进程运行总时间
};

static struct scheduler_stats sched_stats;

// 获取调度器统计信息
void
get_scheduler_stats(struct scheduler_stats *stats)
{
  *stats = sched_stats;
}

// 重置统计信息
void
reset_scheduler_stats(void)
{
  sched_stats.total_switches = 0;
  sched_stats.idle_time = 0;
  sched_stats.run_time = 0;
}


