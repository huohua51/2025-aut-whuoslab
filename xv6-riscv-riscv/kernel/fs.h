
// kernel/fs.h - 文件系统磁盘格式定义

// 这个头文件定义了 xv6 文件系统的磁盘格式
// 内核和用户程序都使用这个头文件
//
// 文件系统特性：
// - 多级索引结构（直接块 + 3级间接块）
// - 支持最大 16GB 的文件
// - 硬链接和软链接支持
// - 增强的元数据（权限、时间戳等）
// - 4KB 块大小优化
//

#ifndef _FS_H_
#define _FS_H_


// 文件系统常量定义


#define ROOTINO  1   // 根目录的 inode 号
#define BSIZE 4096   // 块大小 - 升级为 4KB 以提高性能


// 磁盘布局结构

//
// 磁盘布局：
// [ 引导块 | 超级块 | 日志区 | inode 块 | 空闲位图 | 数据块 ]
//
// 详细布局（4KB 块）：
// 块 0:    引导块
// 块 1:    超级块  
// 块 2-31: 日志区（30 个块）
// 块 32+:  inode 块
// 块 X+:   空闲位图
// 块 Y+:   数据块
//
// mkfs 计算超级块并构建初始文件系统
// 超级块描述磁盘布局：
//
struct superblock {
  uint magic;        // 必须是 FSMAGIC，用于验证文件系统
  uint size;         // 文件系统镜像大小（块数）
  uint nblocks;      // 数据块数量
  uint ninodes;      // inode 数量
  uint nlog;         // 日志块数量
  uint logstart;     // 第一个日志块的块号
  uint inodestart;   // 第一个 inode 块的块号
  uint bmapstart;    // 第一个空闲位图块的块号
};

#define FSMAGIC 0x10203040  // 文件系统魔数，用于识别 xv6 文件系统


// 多级索引结构定义

// 文件索引结构（4级索引系统）：
// addrs[0..9]:   10个直接块（存储小文件的数据）
// addrs[10]:     一级间接块（指向 256 个数据块）
// addrs[11]:     二级间接块（指向 256 个一级间接块）
// addrs[12]:     三级间接块（指向 256 个二级间接块）
//
// 容量计算：
// - 直接块：10 × 4KB = 40KB
// - 一级间接：256 × 4KB = 1MB  
// - 二级间接：256 × 256 × 4KB = 256MB
// - 三级间接：256 × 256 × 256 × 4KB = 16GB
// - 总容量：约 16GB
//
#define NDIRECT 10                                    // 直接块数量
#define NINDIRECT (BSIZE / sizeof(uint))              // 每个间接块包含的条目数（256）
#define NINDIRECT2 (NINDIRECT * NINDIRECT)            // 二级间接块总数（65536）
#define NINDIRECT3 (NINDIRECT * NINDIRECT * NINDIRECT) // 三级间接块总数（16777216）
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT2 + NINDIRECT3)  // 最大文件大小（约16GB）


// 磁盘 inode 结构（增强版）

// 磁盘上的 inode 结构，包含文件的元数据和数据块地址
// 每个 inode 占用 128 字节（4096/32 = 128）
//
struct dinode {
  // 基本文件信息
  short type;           // 文件类型（T_FILE, T_DIR, T_DEVICE, T_SYMLINK）
  short major;          // 主设备号（仅设备文件使用）
  short minor;          // 次设备号（仅设备文件使用）
  short nlink;          // 硬链接数量
  uint size;            // 文件大小（字节）
  uint addrs[NDIRECT+3]; // 数据块地址数组（10个直接 + 3个间接）
  
  // 增强的元数据（现代文件系统特性）
  uint16 mode;          // 文件模式和权限
  uint16 uid;           // 用户 ID
  uint32 atime;         // 最后访问时间
  uint32 mtime;         // 最后修改时间  
  uint32 ctime;         // 创建时间
  uint32 blocks;        // 已分配的数据块数量
  
  // 填充字节，用于对齐到 128 字节边界
  char padding[44];     // 84 + 44 = 128 字节
};


// 文件系统辅助定义


// 每个块包含的 inode 数量
#define IPB           (BSIZE / sizeof(struct dinode))

// 包含 inode i 的块号
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// 每个块的位数
#define BPB           (BSIZE*8)

// 包含块 b 的空闲位图的块号
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)


// 目录结构定义

//
// 目录是一个包含 dirent 结构序列的文件
// 每个目录条目占用 64 字节
//
#define DIRSIZ 62  // 目录条目中文件名的最大长度

// 目录条目结构
// name 字段可以有 DIRSIZ 个字符，不以 NUL 字符结尾
struct dirent {
  ushort inum;                    // inode 号
  char name[DIRSIZ] __attribute__((nonstring));  // 文件名（不以 NUL 结尾）
};

#endif // _FS_H_

