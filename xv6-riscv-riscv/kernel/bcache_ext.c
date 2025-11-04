#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// Thin wrapper layer around xv6 bio to match proposed APIs.
// This maintains compatibility with the existing system while providing
// the enhanced API interface.

struct buffer_head* get_block(uint dev, uint block){
  return (struct buffer_head*)bread(dev, block);
}

void put_block(struct buffer_head *bh){
  brelse((struct buf*)bh);
}

void sync_block(struct buffer_head *bh){
  // Use write-ahead log path when inside a transaction.
  log_block_write((struct buf*)bh);
}

void flush_all_blocks(uint dev){
  // Simple flush: iterate log header during commit; here fall back to
  // nothing since xv6's commit installs all logged blocks.
  (void)dev;
}


