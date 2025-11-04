
// kernel/sysfile.c - 文件系统相关系统调用实现

//
// 此文件实现了与文件系统相关的系统调用
// 包括文件操作、目录操作、路径解析等高级功能
//
// 主要系统调用：
// 1. 文件操作：open, read, write, close, dup, pipe
// 2. 目录操作：mkdir, mknod, chdir, link, unlink
// 3. 路径操作：namei, nameiparent, namex
// 4. 设备操作：major, minor
// 5. 符号链接：symlink, readlink
// 6. 文件状态：fstat, stat
//
// 错误处理：
// - 使用增强的错误码系统
// - 支持错误指针机制
// - 详细的错误信息输出
//
// 安全特性：
// - 参数验证
// - 权限检查
// - 资源清理
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "errno.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}


// 文件描述符操作


// sys_dup - 复制文件描述符
//
// 功能：为已打开的文件创建新的文件描述符
//
// 参数：
//   fd: 要复制的文件描述符
//
// 返回值：
//   成功：返回新的文件描述符
//   失败：返回 -1
//
// 用途：
//   - 实现标准输入/输出重定向
//   - 允许多个文件描述符指向同一个文件
//   - 支持管道和重定向操作
//
uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)  // 获取源文件描述符
    return -1;
  if((fd=fdalloc(f)) < 0)   // 分配新的文件描述符
    return -1;
  filedup(f);               // 增加文件引用计数
  return fd;                // 返回新文件描述符
}

// sys_read - 从文件读取数据
//
// 功能：从指定文件描述符读取数据到用户缓冲区
//
// 参数：
//   fd: 文件描述符
//   buf: 用户空间缓冲区地址
//   n:   要读取的字节数
//
// 返回值：
//   成功：返回实际读取的字节数
//   失败：返回 -1
//
// 支持的文件类型：
//   - 普通文件（FD_INODE）
//   - 设备文件（FD_DEVICE）
//   - 管道（FD_PIPE）
//
uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);  // 获取缓冲区地址
  argint(2, &n);   // 获取读取字节数
  if(argfd(0, 0, &f) < 0)  // 获取文件描述符
    return -1;
  return fileread(f, p, n);  // 调用文件读取函数
}

// sys_write - 向文件写入数据
//
// 功能：将用户缓冲区数据写入指定文件描述符
//
// 参数：
//   fd: 文件描述符
//   buf: 用户空间缓冲区地址
//   n:   要写入的字节数
//
// 返回值：
//   成功：返回实际写入的字节数
//   失败：返回 -1
//
// 支持的文件类型：
//   - 普通文件（FD_INODE）
//   - 设备文件（FD_DEVICE）
//   - 管道（FD_PIPE）
//
uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);  // 获取缓冲区地址
  argint(2, &n);   // 获取写入字节数
  if(argfd(0, 0, &f) < 0)  // 获取文件描述符
    return -1;

  return filewrite(f, p, n);  // 调用文件写入函数
}

// sys_close - 关闭文件描述符
//
// 功能：关闭指定的文件描述符，释放相关资源
//
// 参数：
//   fd: 要关闭的文件描述符
//
// 返回值：
//   成功：返回 0
//   失败：返回 -1
//
// 工作流程：
//   1. 验证文件描述符有效性
//   2. 关闭文件（减少引用计数）
//   3. 释放文件描述符
//
uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)  // 获取文件描述符
    return -1;
  myproc()->ofile[fd] = 0;   // 清除进程文件表项
  fileclose(f);              // 关闭文件
  return 0;                  // 成功
}

// sys_fstat - 获取文件状态信息
//
// 功能：获取指定文件描述符对应的文件状态信息
//
// 参数：
//   fd: 文件描述符
//   st: 用户空间 stat 结构体地址
//
// 返回值：
//   成功：返回 0
//   失败：返回 -1
//
// 返回的信息包括：
//   - 文件类型（普通文件、目录、设备等）
//   - 文件大小
//   - 链接数
//   - 设备号
//   - inode 号
//
uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);  // 获取 stat 结构体地址
  if(argfd(0, 0, &f) < 0)  // 获取文件描述符
    return -1;
  return filestat(f, st);  // 获取文件状态
}


// 硬链接操作


// sys_link - 创建硬链接
//
// 功能：为现有文件创建硬链接（多个文件名指向同一个 inode）
//
// 参数：
//   old: 现有文件路径
//   new: 新链接路径
//
// 返回值：
//   成功：返回 0
//   失败：返回 -1
//
// 硬链接特性：
//   - 多个文件名指向同一个 inode
//   - 不能跨越文件系统边界
//   - 不能链接目录
//   - 删除任一链接不会影响其他链接
//
// 工作流程：
//   1. 解析参数并验证路径
//   2. 查找源文件 inode
//   3. 获取目标目录
//   4. 检查目标名称是否已存在
//   5. 在目标目录中创建链接
//   6. 增加 inode 链接计数
//
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)  // 解析参数
    return -1;

  begin_op();  // 开始文件系统事务
  if((ip = namei(old)) == 0){  // 查找源文件
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){  // 不能链接目录
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;  // 增加链接计数
  iupdate(ip);  // 更新磁盘
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)  // 获取目标目录
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){  // 检查设备并创建链接
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();
  return 0;

bad:
  ilock(ip);
  ip->nlink--;  // 回滚链接计数
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}


// 符号链接操作

// sys_symlink - 创建符号链接
//
// 功能：创建指向目标路径的符号链接
//
// 参数：
//   target: 目标文件路径
//   linkpath: 符号链接路径
//
// 返回值：
//   成功：返回 0
//   失败：返回 -1
//
// 符号链接特性：
//   - 可以跨越文件系统边界
//   - 可以指向不存在的文件
//   - 可以链接目录
//   - 存储目标路径字符串
//
// 工作流程：
//   1. 解析参数并验证路径
//   2. 获取父目录
//   3. 检查链接名是否已存在
//   4. 分配符号链接 inode
//   5. 将目标路径写入 inode 数据
//   6. 在父目录中创建条目
//
uint64
sys_symlink(void)
{
  char target[MAXPATH], linkpath[MAXPATH], name[DIRSIZ];
  struct inode *dp, *ip;

  if(argstr(0, target, MAXPATH) < 0 || argstr(1, linkpath, MAXPATH) < 0)  // 解析参数
    return -1;

  begin_op();  // 开始文件系统事务
  
  // 创建符号链接 inode
  if((dp = nameiparent(linkpath, name)) == 0){  // 获取父目录
    end_op();
    return -1;
  }

  ilock(dp);
  
  // 检查链接名是否已存在
  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    iput(ip);
    end_op();
    return -1;
  }

  // 分配新的符号链接 inode
  if((ip = ialloc(dp->dev, T_SYMLINK)) == 0){
    iunlockput(dp);
    end_op();
    return -1;
  }

  ilock(ip);
  ip->nlink = 1;
  iupdate(ip);

  // 将目标路径写入符号链接 inode 数据
  if(writei(ip, 0, (uint64)target, 0, strlen(target)) != strlen(target)){
    ip->nlink = 0;
    iupdate(ip);
    iunlockput(ip);
    iunlockput(dp);
    end_op();
    return -1;
  }

  // 在父目录中添加条目
  if(dirlink(dp, name, ip->inum) < 0){
    ip->nlink = 0;
    iupdate(ip);
    iunlockput(ip);
    iunlockput(dp);
    end_op();
    return -1;
  }

  iunlockput(dp);
  iunlockput(ip);
  end_op();

  return 0;  // 成功
}

// Read the target of a symbolic link
// sys_readlink - 读取符号链接目标
//
// 功能：读取符号链接文件的目标路径
//
// 参数：
//   path: 符号链接文件路径
//   buf:  用户空间缓冲区地址
//   n:    缓冲区大小
//
// 返回值：
//   成功：返回读取的字节数
//   失败：返回 -1
//
// 工作流程：
//   1. 解析系统调用参数
//   2. 查找符号链接文件
//   3. 验证文件类型
//   4. 读取目标路径到用户缓冲区
//
uint64
sys_readlink(void)
{
  char path[MAXPATH];
  uint64 buf;
  int n;
  struct inode *ip;

  if(argstr(0, path, MAXPATH) < 0)  // 获取路径参数
    return -1;
  argaddr(1, &buf);  // 获取缓冲区地址
  argint(2, &n);     // 获取缓冲区大小

  begin_op();  // 开始文件系统事务
  
  if((ip = namei(path)) == 0){  // 查找符号链接文件
    end_op();
    return -1;
  }

  ilock(ip);
  
  if(ip->type != T_SYMLINK){  // 验证文件类型
    iunlockput(ip);
    end_op();
    return -1;
  }

  // 读取符号链接目标
  int len = ip->size;
  if(len > n)
    len = n;  // 限制读取长度
  
  if(readi(ip, 1, buf, 0, len) != len){  // 读取到用户缓冲区
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();

  return len;  // 返回读取的字节数
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// create - 创建新文件或目录
//
// 功能：在指定路径创建新的文件、目录或设备节点
//
// 参数：
//   path:   文件路径
//   type:   类型（T_FILE, T_DIR, T_DEVICE）
//   major:  主设备号（仅设备节点需要）
//   minor:  次设备号（仅设备节点需要）
//
// 返回值：
//   成功：返回指向新创建 inode 的指针
//   失败：返回错误指针（包含具体错误码）
//
// 错误码：
//   -ENOENT: 父目录不存在
//   -EEXIST: 文件已存在且类型不匹配
//   -EFS_INODE_FULL: inode 分配失败
//   -EIO: 磁盘 I/O 错误
//   -ENOSPC: 目录链接失败（空间不足）
//
// 优化特性：
//   - 支持错误指针机制
//   - 提供详细的错误信息
//   - 自动清理失败时的资源
//
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];
  int error_code = 0;

  // 获取父目录
  if((dp = nameiparent(path, name)) == 0) {
    printf("create: parent directory not found for path '%s'\n", path);
    return ERR_PTR_CODE(-ENOENT);
  }

  ilock(dp);

  // 检查文件是否已存在
  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    
    // 如果是文件且类型匹配，直接返回
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE)) {
      printf("create: file '%s' already exists, returning existing inode\n", name);
      return ip;
    }
    
    // 类型不匹配，返回错误
    printf("create: file '%s' exists but type mismatch (requested %d, existing %d)\n", 
           name, type, ip->type);
    iunlockput(ip);
    return ERR_PTR_CODE(-EEXIST);
  }

  // 分配新的 inode
  ip = ialloc(dp->dev, type);
  if(IS_ERR_PTR(ip)) {
    error_code = PTR_ERR_CODE(ip);
    printf("create: ialloc failed for '%s' with error %d\n", name, error_code);
    iunlockput(dp);
    return ip; // 返回错误指针
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  // 如果是目录，创建 . 和 .. 条目
  if(type == T_DIR){  
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0) {
      printf("create: failed to create . or .. entries for directory '%s'\n", name);
      goto fail_with_error;
    }
  }

  // 在父目录中创建链接
  if(dirlink(dp, name, ip->inum) < 0) {
    printf("create: failed to link '%s' in parent directory\n", name);
    goto fail_with_error;
  }

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);
  
  printf("create: successfully created '%s' (type %d, inode %d)\n", 
         name, type, ip->inum);
  return ip;

 fail_with_error:
  // 清理失败时的资源
  printf("create: cleaning up failed creation of '%s'\n", name);
  
  // 释放 inode
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  
  return ERR_PTR_CODE(-ENOSPC);
}


// 文件操作系统调用


// sys_open - 打开或创建文件
//
// 功能：打开指定路径的文件，支持创建新文件
//
// 参数：
//   path: 文件路径
//   omode: 打开模式（O_RDONLY, O_WRONLY, O_RDWR, O_CREATE 等）
//
// 返回值：
//   成功：返回文件描述符（非负整数）
//   失败：返回错误码（负数）
//
// 错误码：
//   -EINVAL: 无效参数
//   -ENOENT: 文件不存在（非创建模式）
//   -EISDIR: 尝试以写模式打开目录
//   -ENXIO: 无效设备号
//   -EMFILE: 文件描述符不足
//   -ENOSPC: 磁盘空间不足（创建文件时）
//
// 工作流程：
//   1. 解析系统调用参数
//   2. 开始文件系统事务
//   3. 根据模式决定创建或打开文件
//   4. 验证文件类型和权限
//   5. 分配文件描述符
//   6. 设置文件结构
//   7. 结束事务并返回结果
//
uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;
  int error_code = 0;

  // 解析系统调用参数
  argint(1, &omode);  // 获取打开模式
  if((n = argstr(0, path, MAXPATH)) < 0)  // 获取文件路径
    return -EINVAL;

  begin_op();  // 开始文件系统事务

  // 根据模式决定创建或打开文件
  if(omode & O_CREATE){
    // 创建新文件
    ip = create(path, T_FILE, 0, 0);
    if(IS_ERR_PTR(ip)) {
      error_code = PTR_ERR_CODE(ip);
      printf("sys_open: create failed for '%s' with error %d\n", path, error_code);
      end_op();
      return error_code;
    }
  } else {
    // 打开现有文件
    if((ip = namei(path)) == 0){
      printf("sys_open: file '%s' not found\n", path);
      end_op();
      return -ENOENT;
    }
    ilock(ip);
    // 检查是否尝试以写模式打开目录
    if(ip->type == T_DIR && omode != O_RDONLY){
      printf("sys_open: cannot open directory '%s' for writing\n", path);
      iunlockput(ip);
      end_op();
      return -EISDIR;
    }
  }

  // 验证设备文件
  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    printf("sys_open: invalid device major number %d\n", ip->major);
    iunlockput(ip);
    end_op();
    return -ENXIO;
  }

  // 分配文件描述符
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    printf("sys_open: failed to allocate file descriptor for '%s'\n", path);
    iunlockput(ip);
    end_op();
    return -EMFILE;
  }

  // 设置文件结构
  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;  // 重置文件偏移
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);  // 截断文件
  }

  iunlock(ip);
  end_op();

  return fd;  // 返回文件描述符
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  int error_code = 0;

  begin_op();
  
  if(argstr(0, path, MAXPATH) < 0) {
    printf("sys_mkdir: invalid path argument\n");
    end_op();
    return -EINVAL;
  }
  
  ip = create(path, T_DIR, 0, 0);
  if(IS_ERR_PTR(ip)) {
    error_code = PTR_ERR_CODE(ip);
    printf("sys_mkdir: create failed for '%s' with error %d\n", path, error_code);
    end_op();
    return error_code;
  }
  
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;
  int error_code = 0;

  begin_op();
  
  argint(1, &major);
  argint(2, &minor);
  
  if(argstr(0, path, MAXPATH) < 0) {
    printf("sys_mknod: invalid path argument\n");
    end_op();
    return -EINVAL;
  }
  
  ip = create(path, T_DEVICE, major, minor);
  if(IS_ERR_PTR(ip)) {
    error_code = PTR_ERR_CODE(ip);
    printf("sys_mknod: create failed for '%s' with error %d\n", path, error_code);
    end_op();
    return error_code;
  }
  
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
