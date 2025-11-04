// 调度器调试和监控工具
// 提供进程状态监控、性能分析等功能

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include <stdint.h>

// 外部变量声明
extern struct proc proc[NPROC];

// 调度统计信息
struct scheduler_stats {
  uint64 total_switches;
  uint64 total_runtime;
  uint64 idle_time;
  uint64 context_switch_time;
  struct spinlock lock;
};

static struct scheduler_stats sched_stats;

// 进程统计信息
struct proc_stats {
  uint64 runtime;
  uint64 switches;
  uint64 wait_time;
  uint64 last_run;
  int priority;
};

// 全局统计
static struct proc_stats proc_stats[NPROC];

// 初始化调试系统
void
scheduler_debug_init(void)
{
  initlock(&sched_stats.lock, "sched_stats");
  sched_stats.total_switches = 0;
  sched_stats.total_runtime = 0;
  sched_stats.idle_time = 0;
  sched_stats.context_switch_time = 0;
  
  // 初始化进程统计
  for(int i = 0; i < NPROC; i++) {
    proc_stats[i].runtime = 0;
    proc_stats[i].switches = 0;
    proc_stats[i].wait_time = 0;
    proc_stats[i].last_run = 0;
    proc_stats[i].priority = 0;
  }
}

// 更新调度统计
void
update_scheduler_stats(uint64 switch_time, uint64 runtime)
{
  acquire(&sched_stats.lock);
  sched_stats.total_switches++;
  sched_stats.context_switch_time += switch_time;
  sched_stats.total_runtime += runtime;
  release(&sched_stats.lock);
}

// 更新进程统计
void
update_proc_stats(struct proc *p, uint64 runtime, uint64 wait_time)
{
  if(p->pid < 0 || p->pid >= NPROC)
    return;
    
  proc_stats[p->pid].runtime += runtime;
  proc_stats[p->pid].switches++;
  proc_stats[p->pid].wait_time += wait_time;
  proc_stats[p->pid].last_run = 0; // 临时值，需要实现r_time()
}

// 输出进程表调试信息
void
debug_proc_table(void)
{
  printf("=== Process Table Debug ===\n");
  
  for(int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    
    if(p->state != UNUSED) {
      printf("PID: %d, State: %d, Name: %s\n", 
             p->pid, p->state, p->name);
      printf("  Runtime: %lu, Switches: %lu\n",
             proc_stats[i].runtime, proc_stats[i].switches);
      printf("  Wait time: %lu, Priority: %d\n",
             proc_stats[i].wait_time, proc_stats[i].priority);
    }
    
    release(&p->lock);
  }
}

// 输出调度器统计信息
void
debug_scheduler_stats(void)
{
  printf("=== Scheduler Statistics ===\n");
  
  acquire(&sched_stats.lock);
  printf("Total context switches: %lu\n", sched_stats.total_switches);
  printf("Total runtime: %lu\n", sched_stats.total_runtime);
  printf("Total idle time: %lu\n", sched_stats.idle_time);
  printf("Average context switch time: %lu\n", 
         sched_stats.total_switches > 0 ? 
         sched_stats.context_switch_time / sched_stats.total_switches : 0);
  release(&sched_stats.lock);
}

// 分析调度延迟
void
analyze_scheduling_latency(void)
{
  printf("=== Scheduling Latency Analysis ===\n");
  
  uint64 max_latency = 0;
  uint64 min_latency = UINT64_MAX;
  uint64 total_latency = 0;
  int count = 0;
  
  for(int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    
    if(p->state != UNUSED && proc_stats[i].switches > 0) {
      uint64 avg_latency = proc_stats[i].wait_time / proc_stats[i].switches;
      
      if(avg_latency > max_latency)
        max_latency = avg_latency;
      if(avg_latency < min_latency)
        min_latency = avg_latency;
      
      total_latency += avg_latency;
      count++;
    }
    
    release(&p->lock);
  }
  
  if(count > 0) {
    printf("Max scheduling latency: %lu\n", max_latency);
    printf("Min scheduling latency: %lu\n", min_latency);
    printf("Average scheduling latency: %lu\n", total_latency / count);
  }
}

// 检测进程资源泄漏
void
detect_resource_leaks(void)
{
  printf("=== Resource Leak Detection ===\n");
  
  int active_processes = 0;
  int zombie_processes = 0;
  int runnable_processes = 0;
  int sleeping_processes = 0;
  
  for(int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    
    if(p->state != UNUSED) {
      active_processes++;
      
      switch(p->state) {
        case RUNNABLE:
          runnable_processes++;
          break;
        case SLEEPING:
          sleeping_processes++;
          break;
        case ZOMBIE:
          zombie_processes++;
          break;
        case UNUSED:
        case USED:
        case RUNNING:
          // 这些状态不需要特殊处理
          break;
      }
    }
    
    release(&p->lock);
  }
  
  printf("Active processes: %d\n", active_processes);
  printf("Runnable processes: %d\n", runnable_processes);
  printf("Sleeping processes: %d\n", sleeping_processes);
  printf("Zombie processes: %d\n", zombie_processes);
  
  if(zombie_processes > 0) {
    printf("WARNING: %d zombie processes detected!\n", zombie_processes);
  }
}

// 性能分析
void
performance_analysis(void)
{
  printf("=== Performance Analysis ===\n");
  
  uint64 total_cpu_time = 0;
  uint64 total_wait_time = 0;
  int process_count = 0;
  
  for(int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    
    if(p->state != UNUSED) {
      total_cpu_time += proc_stats[i].runtime;
      total_wait_time += proc_stats[i].wait_time;
      process_count++;
    }
    
    release(&p->lock);
  }
  
  if(process_count > 0) {
    printf("Average CPU utilization: %lu%%\n", 
           (total_cpu_time * 100) / (total_cpu_time + total_wait_time));
    printf("Average wait time: %lu\n", total_wait_time / process_count);
    printf("Average runtime: %lu\n", total_cpu_time / process_count);
  }
}

// 调度器健康检查
void
scheduler_health_check(void)
{
  printf("=== Scheduler Health Check ===\n");
  
  int issues = 0;
  
  // 检查是否有死锁
  for(int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    
    if(p->state == RUNNABLE) {
      // 检查进程是否长时间处于可运行状态
      uint64 current_time = 0; // 临时值，需要实现r_time()
      if(current_time - proc_stats[i].last_run > 1000000) { // 1秒
        printf("WARNING: Process %d has been runnable for too long\n", p->pid);
        issues++;
      }
    }
    
    release(&p->lock);
  }
  
  // 检查调度器统计
  acquire(&sched_stats.lock);
  if(sched_stats.total_switches == 0) {
    printf("WARNING: No context switches recorded\n");
    issues++;
  }
  release(&sched_stats.lock);
  
  if(issues == 0) {
    printf("Scheduler health: OK\n");
  } else {
    printf("Scheduler health: %d issues detected\n", issues);
  }
}

// 实时监控
void
real_time_monitoring(void)
{
  printf("=== Real-time Monitoring ===\n");
  
  for(int i = 0; i < 10; i++) { // 监控10个周期
    printf("--- Monitoring cycle %d ---\n", i + 1);
    
    int runnable_count = 0;
    int running_count = 0;
    
    for(int j = 0; j < NPROC; j++) {
      struct proc *p = &proc[j];
      acquire(&p->lock);
      
      if(p->state == RUNNABLE) runnable_count++;
      if(p->state == RUNNING) running_count++;
      
      release(&p->lock);
    }
    
    printf("Runnable processes: %d\n", runnable_count);
    printf("Running processes: %d\n", running_count);
    
    // 使用简单的循环等待，因为sleep需要锁参数
    for(int k = 0; k < 100000; k++) {
      // 简单的延迟循环
    }
  }
}

// 内存使用监控
void
monitor_memory_usage(void)
{
  printf("=== Memory Usage Monitoring ===\n");
  
  // 这里可以添加内存使用统计
  // 由于xv6的内存管理相对简单，主要监控进程数量
  int active_count = 0;
  
  for(int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    acquire(&p->lock);
    
    if(p->state != UNUSED) {
      active_count++;
    }
    
    release(&p->lock);
  }
  
  printf("Active processes: %d/%d\n", active_count, NPROC);
  printf("Process table utilization: %d%%\n", (active_count * 100) / NPROC);
}

// 综合调试报告
void
generate_debug_report(void)
{
  printf("\n========== SCHEDULER DEBUG REPORT ==========\n");
  
  debug_proc_table();
  printf("\n");
  
  debug_scheduler_stats();
  printf("\n");
  
  analyze_scheduling_latency();
  printf("\n");
  
  detect_resource_leaks();
  printf("\n");
  
  performance_analysis();
  printf("\n");
  
  scheduler_health_check();
  printf("\n");
  
  monitor_memory_usage();
  printf("\n");
  
  printf("========== END DEBUG REPORT ==========\n");
}
