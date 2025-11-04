#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void test_basic_create_lookup(void){
  printf("[PATH] create/lookup/delete...\n");
  unlink("p_a.txt");
  int fd = open("p_a.txt", O_CREATE | O_RDWR);
  if(fd < 0){ printf("open create failed\n"); exit(1);} 
  write(fd, "x", 1);
  close(fd);
  fd = open("p_a.txt", O_RDONLY);
  printf("open again: %d\n", fd);
  close(fd);
  unlink("p_a.txt");
}

static void test_deep_path(void){
  printf("[PATH] deep path...\n");
  mkdir("d1"); chdir("d1");
  mkdir("d2"); chdir("d2");
  int fd = open("afile", O_CREATE | O_RDWR);
  write(fd, "y", 1); close(fd);
  chdir("/");
  fd = open("d1/d2/afile", O_RDONLY);
  printf("deep open: %d\n", fd);
  close(fd);
  unlink("d1/d2/afile");
  unlink("d1/d2"); // will fail; not empty
}

static void test_long_name(void){
  printf("[PATH] long name...\n");
  char name[80];
  for(int i=0;i<70;i++) name[i] = (i%26)+'a';
  name[70] = 0;
  unlink(name);
  int fd = open(name, O_CREATE | O_RDWR);
  printf("long open: %d\n", fd);
  if(fd>=0){ close(fd); unlink(name);} 
}

int main(){
  printf("=== Path Tests ===\n");
  test_basic_create_lookup();
  test_deep_path();
  test_long_name();
  printf("Path tests done.\n");
  exit(0);
}


