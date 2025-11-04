// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// 传入小参数：rounds ops_no ops_small ops_big pages_small pages_big
char *argv[] = { "bench_cow", "3", "10", "5", "2", "1", "128", 0 };

int
main(void)
{
  int pid, wpid;

  if(open("console", O_RDWR) < 0){
    mknod("console", CONSOLE, 0);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr


  pid = fork();
  if(pid < 0){
    printf("init: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    exec("bench_cow", argv);
    printf("init: exec bench_cow failed\n");
    exit(1);
  }

  wpid = wait((int *) 0);
  if(wpid == pid){
    printf("init: bench_cow completed, shutting down\n");
    exit(0);
  } else if(wpid < 0){
    printf("init: wait returned an error\n");
    exit(1);
  } else {
    printf("init: unexpected wait result\n");
    exit(1);
  }
}
