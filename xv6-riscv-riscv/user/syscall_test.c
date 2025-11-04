#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

static void test_basic_syscalls(void) {
  printf("Testing basic system calls...\n");

  int pid = getpid();
  printf("Current PID: %d\n", pid);

  int child_pid = fork();
  if (child_pid == 0) {
    // child
    printf("Child process: PID=%d\n", getpid());
    exit(42);
  } else if (child_pid > 0) {
    int status = 0;
    wait(&status);
    printf("Child exited with status: %d\n", status);
  } else {
    printf("Fork failed!\n");
  }
}

static void test_parameter_passing(void) {
  printf("Testing parameter passing...\n");

  char buffer[] = "Hello, World!\n";
  int fd = open("console", O_RDWR);
  if (fd >= 0) {
    int bytes_written = write(fd, buffer, strlen(buffer));
    printf("Wrote %d bytes\n", bytes_written);
    close(fd);
  } else {
    printf("open(console) failed\n");
  }

  // boundary and invalid cases
  write(-1, buffer, 10);           // invalid fd
  write(1, (void*)0, 10);          // null pointer
  write(1, buffer, -1);            // negative length
}

static void test_security(void) {
  printf("Testing security and validation...\n");

  // 1) Invalid user pointer to verify copyin/copyout checks (non-destructive)
  char *invalid_ptr = (char*)0x1000000; // likely unmapped
  int result = write(1, invalid_ptr, 10);
  printf("Invalid pointer write result: %d\n", result);

  // 2) Safe read with guard to ensure no overflow happens in user space
  char buf[16];
  volatile unsigned char guard = 0x5A; // sentinel next to buffer
  result = read(0, buf, sizeof(buf));
  printf("Read returned: %d (guard=%d)\n", result, (int)guard);

  // 3) Invalid destination pointer for read (kernel should fail gracefully)
  result = read(0, (void*)0x2000000, 8);
  printf("Invalid dest pointer read result: %d\n", result);
}

static void test_syscall_performance(void) {
  printf("Testing syscall performance...\n");
  int start = uptime();
  for (int i = 0; i < 10000; i++) {
    getpid();
  }
  int end = uptime();
  printf("10000 getpid() calls took %d ticks\n", end - start);
}

int main(int argc, char **argv) {
  printf("=== Syscall Test Program ===\n");
  test_basic_syscalls();
  test_parameter_passing();
  test_security();
  test_syscall_performance();
  printf("Syscall tests completed.\n");
  exit(0);
}


