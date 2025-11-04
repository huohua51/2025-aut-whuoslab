// Enhanced buffer cache system with hash + LRU management
// This provides a more efficient caching strategy than the original xv6 design

#ifndef BCACHE_ENHANCED_H
#define BCACHE_ENHANCED_H

#include "types.h"
#include "param.h"
#include "sleeplock.h"

// Enhanced buffer head structure
struct buffer_head {
  uint32 block_num;        // Block number
  char *data;                // Data pointer
  int dirty;                 // Dirty bit - needs to be written back
  int ref_count;             // Reference count
  int valid;                 // Valid bit - data has been read from disk
  uint dev;                  // Device number
  struct sleeplock lock;     // Sleep lock for synchronization
  
  // Hash table chain
  struct buffer_head *hash_next;
  struct buffer_head *hash_prev;
  
  // LRU list
  struct buffer_head *lru_next;
  struct buffer_head *lru_prev;
};

// Hash table size (should be a power of 2)
#define HASH_SIZE 256
#define HASH_MASK (HASH_SIZE - 1)

// Function declarations
struct buffer_head* get_block(uint dev, uint block);
void put_block(struct buffer_head *bh);
void sync_block(struct buffer_head *bh);
void flush_all_blocks(uint dev);
void bcache_enhanced_init(void);

// Hash function for block number
static inline uint hash_block(uint dev, uint blockno) {
  return (dev ^ blockno) & HASH_MASK;
}

#endif // BCACHE_ENHANCED_H
