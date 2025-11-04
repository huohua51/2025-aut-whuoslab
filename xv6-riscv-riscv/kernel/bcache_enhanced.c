// Enhanced buffer cache implementation
// Provides hash table + LRU management for better performance

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "bcache_enhanced.h"

// Global cache structures
struct {
  struct spinlock lock;
  struct buffer_head hash_table[HASH_SIZE];
  struct buffer_head lru_head;
  struct buffer_head buffers[NBUF];
} bcache_enhanced;

// Initialize the enhanced buffer cache
void
bcache_enhanced_init(void)
{
  struct buffer_head *b;
  int i;
  
  initlock(&bcache_enhanced.lock, "bcache_enhanced");
  
  // Initialize hash table
  for(i = 0; i < HASH_SIZE; i++) {
    bcache_enhanced.hash_table[i].hash_next = &bcache_enhanced.hash_table[i];
    bcache_enhanced.hash_table[i].hash_prev = &bcache_enhanced.hash_table[i];
  }
  
  // Initialize LRU list
  bcache_enhanced.lru_head.lru_next = &bcache_enhanced.lru_head;
  bcache_enhanced.lru_head.lru_prev = &bcache_enhanced.lru_head;
  
  // Initialize buffer heads
  for(b = bcache_enhanced.buffers; b < bcache_enhanced.buffers + NBUF; b++) {
    initsleeplock(&b->lock, "buffer_head");
    b->data = (char*)kalloc();
    if(b->data == 0)
      panic("bcache_enhanced_init: kalloc failed");
    b->valid = 0;
    b->dirty = 0;
    b->ref_count = 0;
    b->dev = 0;
    b->block_num = 0;
    
    // Add to LRU list
    b->lru_next = bcache_enhanced.lru_head.lru_next;
    b->lru_prev = &bcache_enhanced.lru_head;
    bcache_enhanced.lru_head.lru_next->lru_prev = b;
    bcache_enhanced.lru_head.lru_next = b;
  }
}

// Get a buffer for the specified block
struct buffer_head*
get_block(uint dev, uint block)
{
  struct buffer_head *b;
  uint hash_idx = hash_block(dev, block);
  
  acquire(&bcache_enhanced.lock);
  
  // Look for existing buffer in hash table
  for(b = bcache_enhanced.hash_table[hash_idx].hash_next; 
      b != &bcache_enhanced.hash_table[hash_idx]; 
      b = b->hash_next) {
    if(b->dev == dev && b->block_num == block) {
      b->ref_count++;
      release(&bcache_enhanced.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  // Not found, need to allocate a new buffer
  // Find least recently used buffer
  for(b = bcache_enhanced.lru_head.lru_prev; 
      b != &bcache_enhanced.lru_head; 
      b = b->lru_prev) {
    if(b->ref_count == 0) {
      // Remove from hash table if it was there
      if(b->dev != 0) {
        b->hash_prev->hash_next = b->hash_next;
        b->hash_next->hash_prev = b->hash_prev;
      }
      
      // Set up new buffer
      b->dev = dev;
      b->block_num = block;
      b->valid = 0;
      b->dirty = 0;
      b->ref_count = 1;
      
      // Add to hash table
      b->hash_next = bcache_enhanced.hash_table[hash_idx].hash_next;
      b->hash_prev = &bcache_enhanced.hash_table[hash_idx];
      bcache_enhanced.hash_table[hash_idx].hash_next->hash_prev = b;
      bcache_enhanced.hash_table[hash_idx].hash_next = b;
      
      release(&bcache_enhanced.lock);
      acquiresleep(&b->lock);
      
      // Read from disk if not valid
      if(!b->valid) {
        virtio_disk_rw((struct buf*)b, 0);  // Cast to original buf for compatibility
        b->valid = 1;
      }
      
      return b;
    }
  }
  
  panic("get_block: no free buffers");
  return 0;
}

// Release a buffer
void
put_block(struct buffer_head *bh)
{
  if(!holdingsleep(&bh->lock))
    panic("put_block: buffer not locked");
  
  releasesleep(&bh->lock);
  
  acquire(&bcache_enhanced.lock);
  bh->ref_count--;
  
  if(bh->ref_count == 0) {
    // Move to head of LRU list (most recently used)
    bh->lru_prev->lru_next = bh->lru_next;
    bh->lru_next->lru_prev = bh->lru_prev;
    bh->lru_next = bcache_enhanced.lru_head.lru_next;
    bh->lru_prev = &bcache_enhanced.lru_head;
    bcache_enhanced.lru_head.lru_next->lru_prev = bh;
    bcache_enhanced.lru_head.lru_next = bh;
  }
  
  release(&bcache_enhanced.lock);
}

// Sync a buffer to disk
void
sync_block(struct buffer_head *bh)
{
  if(!holdingsleep(&bh->lock))
    panic("sync_block: buffer not locked");
  
  if(bh->dirty) {
    virtio_disk_rw((struct buf*)bh, 1);  // Write to disk
    bh->dirty = 0;
  }
}

// Flush all dirty blocks for a device
void
flush_all_blocks(uint dev)
{
  struct buffer_head *b;
  int i;
  
  acquire(&bcache_enhanced.lock);
  
  for(i = 0; i < HASH_SIZE; i++) {
    for(b = bcache_enhanced.hash_table[i].hash_next; 
        b != &bcache_enhanced.hash_table[i]; 
        b = b->hash_next) {
      if(b->dev == dev && b->dirty) {
        acquiresleep(&b->lock);
        virtio_disk_rw((struct buf*)b, 1);
        b->dirty = 0;
        releasesleep(&b->lock);
      }
    }
  }
  
  release(&bcache_enhanced.lock);
}
