// 进程同步原语实现
// 基于xv6的sleep/wakeup机制，实现信号量、条件变量等

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// 信号量结构
struct semaphore {
  int value;
  struct spinlock lock;
  char name[16];
};

// 条件变量结构
struct condition {
  struct spinlock lock;
  int waiters;
  char name[16];
};

// 互斥锁结构
struct mutex {
  int locked;
  struct proc *owner;
  struct spinlock lock;
  char name[16];
};

// 读写锁结构
struct rwlock {
  int readers;
  int writer;
  struct proc *owner;
  struct spinlock lock;
  char name[16];
};

// 定义常量
#define NSEM 32
#define NCOND 32
#define NMUTEX 32
#define NRWLOCK 32

// 全局同步原语表
static struct semaphore semaphores[NSEM];
static struct condition conditions[NCOND];
static struct mutex mutexes[NMUTEX];
static struct rwlock rwlocks[NRWLOCK];

// 初始化同步原语
void
sync_init(void)
{
  // 初始化信号量
  for(int i = 0; i < NSEM; i++) {
    initlock(&semaphores[i].lock, "semaphore");
    semaphores[i].value = 0;
    semaphores[i].name[0] = 0;
  }
  
  // 初始化条件变量
  for(int i = 0; i < NCOND; i++) {
    initlock(&conditions[i].lock, "condition");
    conditions[i].waiters = 0;
    conditions[i].name[0] = 0;
  }
  
  // 初始化互斥锁
  for(int i = 0; i < NMUTEX; i++) {
    initlock(&mutexes[i].lock, "mutex");
    mutexes[i].locked = 0;
    mutexes[i].owner = 0;
    mutexes[i].name[0] = 0;
  }
  
  // 初始化读写锁
  for(int i = 0; i < NRWLOCK; i++) {
    initlock(&rwlocks[i].lock, "rwlock");
    rwlocks[i].readers = 0;
    rwlocks[i].writer = 0;
    rwlocks[i].name[0] = 0;
  }
}

// 信号量操作
int
sem_init(int sem_id, int initial_value)
{
  if(sem_id < 0 || sem_id >= NSEM)
    return -1;
    
  acquire(&semaphores[sem_id].lock);
  semaphores[sem_id].value = initial_value;
  release(&semaphores[sem_id].lock);
  
  return 0;
}

int
sem_wait(int sem_id)
{
  if(sem_id < 0 || sem_id >= NSEM)
    return -1;
    
  acquire(&semaphores[sem_id].lock);
  
  while(semaphores[sem_id].value <= 0) {
    // 使用sleep等待信号量
    sleep(&semaphores[sem_id], &semaphores[sem_id].lock);
  }
  
  semaphores[sem_id].value--;
  release(&semaphores[sem_id].lock);
  
  return 0;
}

int
sem_post(int sem_id)
{
  if(sem_id < 0 || sem_id >= NSEM)
    return -1;
    
  acquire(&semaphores[sem_id].lock);
  semaphores[sem_id].value++;
  
  // 唤醒等待的进程
  wakeup(&semaphores[sem_id]);
  release(&semaphores[sem_id].lock);
  
  return 0;
}

// 互斥锁操作
int
mutex_init(int mutex_id)
{
  if(mutex_id < 0 || mutex_id >= NMUTEX)
    return -1;
    
  acquire(&mutexes[mutex_id].lock);
  mutexes[mutex_id].locked = 0;
  mutexes[mutex_id].owner = 0;
  release(&mutexes[mutex_id].lock);
  
  return 0;
}

int
mutex_lock(int mutex_id)
{
  if(mutex_id < 0 || mutex_id >= NMUTEX)
    return -1;
    
  struct proc *p = myproc();
  
  acquire(&mutexes[mutex_id].lock);
  
  while(mutexes[mutex_id].locked && mutexes[mutex_id].owner != p) {
    sleep(&mutexes[mutex_id], &mutexes[mutex_id].lock);
  }
  
  mutexes[mutex_id].locked = 1;
  mutexes[mutex_id].owner = p;
  release(&mutexes[mutex_id].lock);
  
  return 0;
}

int
mutex_unlock(int mutex_id)
{
  if(mutex_id < 0 || mutex_id >= NMUTEX)
    return -1;
    
  struct proc *p = myproc();
  
  acquire(&mutexes[mutex_id].lock);
  
  if(mutexes[mutex_id].owner != p) {
    release(&mutexes[mutex_id].lock);
    return -1; // 不是锁的拥有者
  }
  
  mutexes[mutex_id].locked = 0;
  mutexes[mutex_id].owner = 0;
  
  // 唤醒等待的进程
  wakeup(&mutexes[mutex_id]);
  release(&mutexes[mutex_id].lock);
  
  return 0;
}

// 条件变量操作
int
cond_init(int cond_id)
{
  if(cond_id < 0 || cond_id >= NCOND)
    return -1;
    
  acquire(&conditions[cond_id].lock);
  conditions[cond_id].waiters = 0;
  release(&conditions[cond_id].lock);
  
  return 0;
}

int
cond_wait(int cond_id, int mutex_id)
{
  if(cond_id < 0 || cond_id >= NCOND)
    return -1;
    
  acquire(&conditions[cond_id].lock);
  conditions[cond_id].waiters++;
  
  // 释放互斥锁并等待
  mutex_unlock(mutex_id);
  sleep(&conditions[cond_id], &conditions[cond_id].lock);
  
  // 重新获取互斥锁
  mutex_lock(mutex_id);
  conditions[cond_id].waiters--;
  release(&conditions[cond_id].lock);
  
  return 0;
}

int
cond_signal(int cond_id)
{
  if(cond_id < 0 || cond_id >= NCOND)
    return -1;
    
  acquire(&conditions[cond_id].lock);
  
  if(conditions[cond_id].waiters > 0) {
    wakeup(&conditions[cond_id]);
  }
  
  release(&conditions[cond_id].lock);
  return 0;
}

int
cond_broadcast(int cond_id)
{
  if(cond_id < 0 || cond_id >= NCOND)
    return -1;
    
  acquire(&conditions[cond_id].lock);
  
  if(conditions[cond_id].waiters > 0) {
    wakeup(&conditions[cond_id]);
  }
  
  release(&conditions[cond_id].lock);
  return 0;
}

// 读写锁操作
int
rwlock_init(int rwlock_id)
{
  if(rwlock_id < 0 || rwlock_id >= NRWLOCK)
    return -1;
    
  acquire(&rwlocks[rwlock_id].lock);
  rwlocks[rwlock_id].readers = 0;
  rwlocks[rwlock_id].writer = 0;
  release(&rwlocks[rwlock_id].lock);
  
  return 0;
}

int
rwlock_read_lock(int rwlock_id)
{
  if(rwlock_id < 0 || rwlock_id >= NRWLOCK)
    return -1;
    
  acquire(&rwlocks[rwlock_id].lock);
  
  while(rwlocks[rwlock_id].writer) {
    sleep(&rwlocks[rwlock_id], &rwlocks[rwlock_id].lock);
  }
  
  rwlocks[rwlock_id].readers++;
  release(&rwlocks[rwlock_id].lock);
  
  return 0;
}

int
rwlock_read_unlock(int rwlock_id)
{
  if(rwlock_id < 0 || rwlock_id >= NRWLOCK)
    return -1;
    
  acquire(&rwlocks[rwlock_id].lock);
  rwlocks[rwlock_id].readers--;
  
  if(rwlocks[rwlock_id].readers == 0) {
    wakeup(&rwlocks[rwlock_id]);
  }
  
  release(&rwlocks[rwlock_id].lock);
  return 0;
}

int
rwlock_write_lock(int rwlock_id)
{
  if(rwlock_id < 0 || rwlock_id >= NRWLOCK)
    return -1;
    
  struct proc *p = myproc();
  
  acquire(&rwlocks[rwlock_id].lock);
  
  while(rwlocks[rwlock_id].writer || rwlocks[rwlock_id].readers > 0) {
    sleep(&rwlocks[rwlock_id], &rwlocks[rwlock_id].lock);
  }
  
  rwlocks[rwlock_id].writer = 1;
  rwlocks[rwlock_id].owner = p;
  release(&rwlocks[rwlock_id].lock);
  
  return 0;
}

int
rwlock_write_unlock(int rwlock_id)
{
  if(rwlock_id < 0 || rwlock_id >= NRWLOCK)
    return -1;
    
  struct proc *p = myproc();
  
  acquire(&rwlocks[rwlock_id].lock);
  
  if(rwlocks[rwlock_id].owner != p) {
    release(&rwlocks[rwlock_id].lock);
    return -1; // 不是锁的拥有者
  }
  
  rwlocks[rwlock_id].writer = 0;
  rwlocks[rwlock_id].owner = 0;
  
  wakeup(&rwlocks[rwlock_id]);
  release(&rwlocks[rwlock_id].lock);
  
  return 0;
}

// 生产者-消费者问题的实现
#define BUFFER_SIZE 10
struct producer_consumer {
  int buffer[BUFFER_SIZE];
  int in, out, count;
  struct spinlock lock;
  struct semaphore empty, full;
};

static struct producer_consumer pc_buffer;

void
pc_init(void)
{
  initlock(&pc_buffer.lock, "pc_buffer");
  pc_buffer.in = pc_buffer.out = pc_buffer.count = 0;
  sem_init(0, BUFFER_SIZE); // empty semaphore
  sem_init(1, 0);           // full semaphore
}

void
producer(void)
{
  int item = 0;
  
  while(1) {
    // 生产一个项目
    item++;
    
    // 等待空位
    sem_wait(0); // empty
    
    acquire(&pc_buffer.lock);
    pc_buffer.buffer[pc_buffer.in] = item;
    pc_buffer.in = (pc_buffer.in + 1) % BUFFER_SIZE;
    pc_buffer.count++;
    release(&pc_buffer.lock);
    
    // 通知有新产品
    sem_post(1); // full
    
    // 模拟生产时间
    for(int i = 0; i < 1000; i++);
  }
}

void
consumer(void)
{
  int item;
  
  while(1) {
    // 等待产品
    sem_wait(1); // full
    
    acquire(&pc_buffer.lock);
    item = pc_buffer.buffer[pc_buffer.out];
    pc_buffer.out = (pc_buffer.out + 1) % BUFFER_SIZE;
    pc_buffer.count--;
    release(&pc_buffer.lock);
    
    // 通知有空位
    sem_post(0); // empty
    
    // 消费产品
    printf("Consumed item: %d\n", item);
    
    // 模拟消费时间
    for(int i = 0; i < 1000; i++);
  }
}
