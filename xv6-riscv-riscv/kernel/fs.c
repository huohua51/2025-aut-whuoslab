
// kernel/fs.c - 文件系统底层实现

//
// 文件系统实现。五个层次：
//   + 块管理：原始磁盘块的分配器
//   + 日志：多步更新的崩溃恢复
//   + 文件：inode 分配器、读取、写入、元数据
//   + 目录：具有特殊内容的 inode（其他 inode 的列表！）
//   + 名称：如 /usr/rtm/xv6/fs.c 的路径，便于命名
//
// 此文件包含底层文件系统操作例程
// （更高级的）系统调用实现在 sysfile.c 中
//
// 主要功能模块：
// 1. 块分配和释放（balloc, bfree）
// 2. inode 管理（ialloc, iget, iput, iupdate）
// 3. 多级索引（bmap, itrunc）
// 4. 文件读写（readi, writei）
// 5. 目录操作（dirlookup, dirlink）
// 6. 路径解析（namex, namei, nameiparent）
// 7. 符号链接支持（T_SYMLINK）
// 8. 增强的元数据管理
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "errno.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
  ireclaim(dev);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_block_write(bp);
  brelse(bp);
}


// 块分配和释放


// balloc - 分配一个清零的磁盘块
//
// 功能：在指定设备上分配一个空闲的数据块
//
// 参数：
//   dev: 设备号
//
// 返回值：
//   成功：返回分配的块号
//   失败：返回 0（磁盘空间不足）
//
// 工作流程：
//   1. 遍历所有位图块
//   2. 在位图中查找空闲位
//   3. 标记块为已使用
//   4. 清零新分配的块
//   5. 返回块号
//
// 优化特性：
//   - 使用位图快速查找空闲块
//   - 自动清零新分配的块
//   - 支持大容量文件系统
//
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  // 遍历所有位图块
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    // 在当前位图块中查找空闲位
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);  // 计算位掩码
      if((bp->data[bi/8] & m) == 0){  // 检查块是否空闲
        bp->data[bi/8] |= m;  // 标记块为已使用
        log_block_write(bp);   // 写入日志
        brelse(bp);
        bzero(dev, b + bi);    // 清零新分配的块
        return b + bi;        // 返回块号
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// bfree - 释放一个磁盘块
//
// 功能：将指定的数据块标记为空闲
//
// 参数：
//   dev: 设备号
//   b:   要释放的块号
//
// 工作流程：
//   1. 计算块在位图中的位置
//   2. 读取对应的位图块
//   3. 清除对应的位
//   4. 写回位图块
//
// 安全特性：
//   - 检查块号的有效性
//   - 使用日志确保原子性
//
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  // 读取包含块 b 的位图块
  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;           // 块在位图中的索引
  m = 1 << (bi % 8);     // 位掩码
  if((bp->data[bi/8] & m) == 0)  // 检查块是否已分配
    panic("freeing free block");
  bp->data[bi/8] &= ~m;  // 清除位，标记为空闲
  log_block_write(bp);   // 写入日志
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void
iinit()
{
  int i = 0;
  
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// ialloc - 分配一个新的 inode
//
// 功能：在指定设备上分配一个指定类型的空闲 inode
//
// 参数：
//   dev:  设备号
//   type: inode 类型（T_FILE, T_DIR, T_DEVICE）
//
// 返回值：
//   成功：返回指向新分配 inode 的指针
//   失败：返回错误指针（包含具体错误码）
//
// 错误码：
//   -ENOSPC: 没有可用的 inode（文件系统已满）
//   -EIO:    磁盘 I/O 错误
//   -EFS_CORRUPT: 文件系统损坏
//
// 优化特性：
//   - 返回具体错误码而不是简单的 NULL
//   - 提供详细的错误信息
//   - 支持错误指针机制
//
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  // 验证参数
  if(type != T_FILE && type != T_DIR && type != T_DEVICE) {
    printf("ialloc: invalid type %d\n", type);
    return ERR_PTR_CODE(-EINVAL);
  }

  // 尝试分配 inode，支持重试机制
  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    if(bp == 0) {
      printf("ialloc: failed to read inode block %ld\n", IBLOCK(inum, sb));
      return ERR_PTR_CODE(-EIO);
    }
    
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // 找到空闲的 inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      
      // Initialize enhanced metadata
      dip->mode = 0644;  // Default permissions: rw-r--r--
      dip->uid = 0;      // Default to root user
      dip->atime = 0;    // Will be set on first access
      dip->mtime = 0;    // Will be set on first modification
      dip->ctime = 0;    // Will be set on creation
      dip->blocks = 0;   // No blocks allocated yet
      
      // Initialize padding
      memset(dip->padding, 0, sizeof(dip->padding));
      
      // 写入磁盘（log_block_write 不返回错误码）
      log_block_write(bp);
      
      brelse(bp);
      
      // 获取内存中的 inode 结构
      struct inode *ip = iget(dev, inum);
      if(ip == 0) {
        printf("ialloc: iget failed for inode %d\n", inum);
        return ERR_PTR_CODE(-EIO);
      }
      
      printf("ialloc: allocated inode %d (type %d)\n", inum, type);
      return ip;
    }
    brelse(bp);
  }
  
  // 没有找到空闲 inode
  printf("ialloc: no free inodes available (checked %d inodes)\n", sb.ninodes - 1);
  return ERR_PTR_CODE(-EFS_INODE_FULL);
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  
  // Save enhanced metadata
  dip->mode = ip->mode;
  dip->uid = ip->uid;
  dip->atime = ip->atime;
  dip->mtime = ip->mtime;
  dip->ctime = ip->ctime;
  dip->blocks = ip->blocks;
  
  // Copy padding (not used in memory, but needed for disk alignment)
  memmove(dip->padding, ip->padding, sizeof(dip->padding));
  
  log_block_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&itable.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    
    // Load enhanced metadata
    ip->mode = dip->mode;
    ip->uid = dip->uid;
    ip->atime = dip->atime;
    ip->mtime = dip->mtime;
    ip->ctime = dip->ctime;
    ip->blocks = dip->blocks;
    
    // Copy padding (not used in memory, but needed for disk alignment)
    memmove(ip->padding, dip->padding, sizeof(ip->padding));
    
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

void
ireclaim(int dev)
{
  for (int inum = 1; inum < sb.ninodes; inum++) {
    struct inode *ip = 0;
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    if (dip->type != 0 && dip->nlink == 0) {  // is an orphaned inode
      printf("ireclaim: orphaned inode %d\n", inum);
      ip = iget(dev, inum);
    }
    brelse(bp);
    if (ip) {
      begin_op();
      ilock(ip);
      iunlock(ip);
      iput(ip);
      end_op();
    }
  }
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// 多级索引实现


// bmap - 返回 inode 中第 bn 个块的磁盘块地址
//
// 功能：实现多级索引结构，支持最大 16GB 的文件
//
// 参数：
//   ip: 指向 inode 的指针
//   bn: 逻辑块号（从 0 开始）
//
// 返回值：
//   成功：返回对应的磁盘块地址
//   失败：返回 0（磁盘空间不足）
//
// 索引结构：
//   - 直接块（0-9）：直接存储数据块地址
//   - 一级间接（10-265）：通过一个间接块指向数据块
//   - 二级间接（266-65701）：通过两个间接块指向数据块
//   - 三级间接（65702-16777216）：通过三个间接块指向数据块
//
// 优化特性：
//   - 按需分配：只有在需要时才分配间接块
//   - 缓存友好：小文件使用直接块，访问效率高
//   - 扩展性强：支持从 KB 到 GB 的文件大小
//
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp, *bp2, *bp3;

  // 1. 直接块处理（0-9）
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      addr = balloc(ip->dev);  // 分配新数据块
      if(addr == 0)
        return 0;
      ip->addrs[bn] = addr;   // 保存块地址
    }
    return addr;
  }
  bn -= NDIRECT;

  // 2. 一级间接块处理（10-265）
  if(bn < NINDIRECT){
    // 加载间接块，必要时分配
    if((addr = ip->addrs[NDIRECT]) == 0){
      addr = balloc(ip->dev);  // 分配间接块
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT] = addr;
    }
    bp = bread(ip->dev, addr);  // 读取间接块
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      addr = balloc(ip->dev);   // 分配数据块
      if(addr){
        a[bn] = addr;           // 保存数据块地址
        log_block_write(bp);    // 写回间接块
      }
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  // 3. 二级间接块处理（266-65701）
  if(bn < NINDIRECT2){
    // 加载二级间接块
    if((addr = ip->addrs[NDIRECT+1]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT+1] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    
    // 获取一级间接块
    if((addr = a[bn / NINDIRECT]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[bn / NINDIRECT] = addr;
        log_block_write(bp);
      }
    }
    brelse(bp);
    if(addr == 0)
      return 0;
    
    // 从一级间接块获取数据块
    bp2 = bread(ip->dev, addr);
    a = (uint*)bp2->data;
    if((addr = a[bn % NINDIRECT]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[bn % NINDIRECT] = addr;
        log_block_write(bp2);
      }
    }
    brelse(bp2);
    return addr;
  }
  bn -= NINDIRECT2;

  // 4. 三级间接块处理（65702-16777216）
  if(bn < NINDIRECT3){
    // 加载三级间接块
    if((addr = ip->addrs[NDIRECT+2]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT+2] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    
    // 获取二级间接块
    if((addr = a[bn / NINDIRECT2]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[bn / NINDIRECT2] = addr;
        log_block_write(bp);
      }
    }
    brelse(bp);
    if(addr == 0)
      return 0;
    
    // 从二级间接块获取一级间接块
    bp2 = bread(ip->dev, addr);
    a = (uint*)bp2->data;
    if((addr = a[(bn % NINDIRECT2) / NINDIRECT]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[(bn % NINDIRECT2) / NINDIRECT] = addr;
        log_block_write(bp2);
      }
    }
    brelse(bp2);
    if(addr == 0)
      return 0;
    
    // 从一级间接块获取数据块
    bp3 = bread(ip->dev, addr);
    a = (uint*)bp3->data;
    if((addr = a[bn % NINDIRECT]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[bn % NINDIRECT] = addr;
        log_block_write(bp3);
      }
    }
    brelse(bp3);
    return addr;
  }

  panic("bmap: out of range");  // 超出最大文件大小
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j, k, l;
  struct buf *bp, *bp2, *bp3;
  uint *a, *a2, *a3;

  // Free direct blocks
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  // Free single indirect block
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  // Free double indirect block
  if(ip->addrs[NDIRECT+1]){
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]){
        // Free single indirect block
        bp2 = bread(ip->dev, a[j]);
        a2 = (uint*)bp2->data;
        for(k = 0; k < NINDIRECT; k++){
          if(a2[k])
            bfree(ip->dev, a2[k]);
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }

  // Free triple indirect block
  if(ip->addrs[NDIRECT+2]){
    bp = bread(ip->dev, ip->addrs[NDIRECT+2]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]){
        // Free double indirect block
        bp2 = bread(ip->dev, a[j]);
        a2 = (uint*)bp2->data;
        for(k = 0; k < NINDIRECT; k++){
          if(a2[k]){
            // Free single indirect block
            bp3 = bread(ip->dev, a2[k]);
            a3 = (uint*)bp3->data;
            for(l = 0; l < NINDIRECT; l++){
              if(a3[l])
                bfree(ip->dev, a3[l]);
            }
            brelse(bp3);
            bfree(ip->dev, a2[k]);
          }
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+2]);
    ip->addrs[NDIRECT+2] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Update access time for inode
// Caller must hold ip->lock.
void
iupdatatime(struct inode *ip)
{
  // Simple time counter - in real implementation would use actual time
  static uint32 current_time = 0;
  current_time++;
  ip->atime = current_time;
}

// Update modification time for inode
// Caller must hold ip->lock.
void
iupdatemtime(struct inode *ip)
{
  static uint32 current_time = 0;
  current_time++;
  ip->mtime = current_time;
}

// Update creation time for inode
// Caller must hold ip->lock.
void
iupdatectime(struct inode *ip)
{
  static uint32 current_time = 0;
  current_time++;
  ip->ctime = current_time;
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  
  // Update access time
  if(tot > 0) {
    iupdatatime(ip);
  }
  
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_block_write(bp);
    brelse(bp);
  }

  if(off > ip->size)
    ip->size = off;

  // Update modification time
  if(tot > 0) {
    iupdatemtime(ip);
  }

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  char symlink_target[MAXPATH];
  int symlink_depth = 0;
  #define MAX_SYMLINK_DEPTH 16  // Prevent infinite loops

  if(path == 0 || *path == 0){
    return 0;
  }

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
    
    // Follow symbolic links
    ilock(ip);
    if(ip->type == T_SYMLINK){
      // Check for too many levels of symlinks (potential loop)
      if(++symlink_depth > MAX_SYMLINK_DEPTH){
        iunlockput(ip);
        return 0;  // ELOOP would be set here
      }
      
      // Read the symlink target
      if(readi(ip, 0, (uint64)symlink_target, 0, MAXPATH) <= 0){
        iunlockput(ip);
        return 0;
      }
      symlink_target[ip->size] = '\0';  // Ensure null termination
      iunlockput(ip);
      iput(ip);
      
      // Restart path lookup from symlink target
      if(symlink_target[0] == '/'){
        // Absolute path
        ip = iget(ROOTDEV, ROOTINO);
      } else {
        // Relative path - need to go back to parent directory
        // For simplicity, we don't support relative symlinks in this implementation
        // A full implementation would need to track the parent directory
        return 0;
      }
      
      // Continue parsing from symlink target
      char *newpath = symlink_target;
      while((newpath = skipelem(newpath, name)) != 0){
        ilock(ip);
        if(ip->type != T_DIR){
          iunlockput(ip);
          return 0;
        }
        if((next = dirlookup(ip, name, 0)) == 0){
          iunlockput(ip);
          return 0;
        }
        iunlockput(ip);
        ip = next;
      }
      
      // Now continue with the rest of the original path
      continue;
    }
    iunlock(ip);
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

// Lightweight wrappers to align with higher-level API naming
struct inode* path_walk(char *path) {
  return namei(path);
}

struct inode* path_parent(char *path, char *name) {
  return nameiparent(path, name);
}

// Non-panicking lookup returning explicit status codes.
// Returns 0 on success, negative on error:
// -1: not a directory, -2: not found, -3: I/O error
int dir_lookup_ex(struct inode *dp, char *name, uint *poff, struct inode **out)
{
  uint off;
  struct dirent de;
  if(dp->type != T_DIR)
    return -1;
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      return -3;
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      if(poff) *poff = off;
      *out = iget(dp->dev, de.inum);
      return 0;
    }
  }
  return -2;
}

// Enhanced directory operations with better error handling
int dir_create_entry(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;
  
  // Check if entry already exists
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1; // Entry already exists
  }
  
  // Find empty slot
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      return -2; // I/O error
    if(de.inum == 0)
      break;
  }
  
  // Create new entry
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -3; // Write error
    
  return 0; // Success
}

int dir_remove_entry(struct inode *dp, char *name)
{
  uint off;
  struct dirent de;
  struct inode *ip;
  
  // Find the entry
  if((ip = dirlookup(dp, name, &off)) == 0)
    return -1; // Not found
    
  iput(ip);
  
  // Clear the entry
  de.inum = 0;
  memset(de.name, 0, DIRSIZ);
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -2; // Write error
    
  return 0; // Success
}

// Enhanced path resolution with better error handling
struct inode* path_resolve(char *path, int *error)
{
  char name[DIRSIZ];
  struct inode *ip, *next;
  char *p = path;
  
  if(path == 0 || *path == 0){
    if(error) *error = -1; // Invalid path
    return 0;
  }
  
  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);
    
  while((p = skipelem(p, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      if(error) *error = -2; // Not a directory
      return 0;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      if(error) *error = -3; // Not found
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  
  if(error) *error = 0; // Success
  return ip;
}
