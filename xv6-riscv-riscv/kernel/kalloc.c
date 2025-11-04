// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// COW: Reference counting for physical pages
struct {
  struct spinlock lock;
  int count[PHYSTOP / PGSIZE];  // Reference count for each physical page
} pg_refcnt;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&pg_refcnt.lock, "pg_refcnt");
  freerange(end, (void*)PHYSTOP);
}

// Get page index from physical address
static inline int
pa2idx(uint64 pa)
{
  return pa / PGSIZE;
}

// Increment reference count for a physical page
void
krefpage(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    return;
    
  acquire(&pg_refcnt.lock);
  pg_refcnt.count[pa2idx((uint64)pa)]++;
  release(&pg_refcnt.lock);
}

// Decrement reference count and free if reaches 0
// Returns 1 if page was freed, 0 otherwise
int
kunrefpage(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    return 0;
    
  acquire(&pg_refcnt.lock);
  int idx = pa2idx((uint64)pa);
  
  if(pg_refcnt.count[idx] < 1) {
    panic("kunrefpage: refcount < 1");
  }
  
  pg_refcnt.count[idx]--;
  int should_free = (pg_refcnt.count[idx] == 0);
  release(&pg_refcnt.lock);
  
  if(should_free) {
    kfree(pa);
    return 1;
  }
  return 0;
}

// Get reference count (for debugging)
int
krefcount(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    return 0;
    
  acquire(&pg_refcnt.lock);
  int cnt = pg_refcnt.count[pa2idx((uint64)pa)];
  release(&pg_refcnt.lock);
  return cnt;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // COW: Set reference count to 0
  acquire(&pg_refcnt.lock);
  pg_refcnt.count[pa2idx((uint64)pa)] = 0;
  release(&pg_refcnt.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    
    // COW: Initialize reference count to 1
    acquire(&pg_refcnt.lock);
    pg_refcnt.count[pa2idx((uint64)r)] = 1;
    release(&pg_refcnt.lock);
  }
  
  return (void*)r;
}
