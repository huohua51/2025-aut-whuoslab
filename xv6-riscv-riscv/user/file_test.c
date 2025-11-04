#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define ASSERT(expr) do { \
  if(!(expr)) { \
    printf("assert failed: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

static void make_name(const char *prefix, int num, char *out, int outsz) {
  // simple decimal append without snprintf
  int i = 0;
  while(prefix[i] && i < outsz - 1) { out[i] = prefix[i]; i++; }
  // convert num to string (reverse)
  char tmp[16];
  int t = 0;
  if(num == 0) { tmp[t++] = '0'; }
  while(num > 0 && t < (int)sizeof(tmp)) { tmp[t++] = '0' + (num % 10); num /= 10; }
  // append reversed
  while(t > 0 && i < outsz - 1) { out[i++] = tmp[--t]; }
  out[i] = '\0';
}

static inline uint64 get_time(void) { return (uint64)uptime(); }

static void test_filesystem_integrity(void) {
  printf("[FS] integrity test...\n");
  int fd = open("testfile", O_CREATE | O_RDWR);
  ASSERT(fd >= 0);
  char buffer[] = "Hello, filesystem!";
  int bytes = write(fd, buffer, strlen(buffer));
  ASSERT(bytes == (int)strlen(buffer));
  close(fd);

  fd = open("testfile", O_RDONLY);
  ASSERT(fd >= 0);
  char read_buffer[64];
  bytes = read(fd, read_buffer, sizeof(read_buffer)-1);
  ASSERT(bytes >= 0);
  read_buffer[bytes] = '\0';
  ASSERT(strcmp(buffer, read_buffer) == 0);
  close(fd);
  ASSERT(unlink("testfile") == 0);
  printf("[FS] integrity passed\n");
}

static void test_concurrent_access(void) {
  printf("[FS] concurrent access...\n");
  for (int i = 0; i < 4; i++) {
    int pid = fork();
    if (pid == 0) {
      char filename[32];
      make_name("test_", i, filename, sizeof(filename));
      for (int j = 0; j < 100; j++) {
        int fd = open(filename, O_CREATE | O_RDWR);
        if (fd >= 0) {
          write(fd, &j, sizeof(j));
          close(fd);
          unlink(filename);
        }
      }
      exit(0);
    }
  }
  for (int i = 0; i < 4; i++) { wait(0); }
  printf("[FS] concurrent access done\n");
}

static void test_filesystem_performance(void) {
  printf("[FS] performance...\n");
  uint64 start = get_time();
  // many small files (with error checks)
  int created = 0;
  for (int i = 0; i < 100; i++) {
    char filename[32];
    make_name("small_", i, filename, sizeof(filename));
    int fd = open(filename, O_CREATE | O_RDWR);
    if (fd < 0) { printf("open failed at %s\n", filename); break; }
    int w = write(fd, "test", 4);
    if (w != 4) { printf("write short %d at %s\n", w, filename); close(fd); break; }
    close(fd);
    created++;
    if ((i % 25) == 24) printf("[FS] created %d small files...\n", created);
  }
  uint64 small_time = get_time() - start;

  // one large file, write in 1KB chunks using heap buffer (total ~512KB)
  start = get_time();
  int fd = open("large_file", O_CREATE | O_RDWR);
  ASSERT(fd >= 0);
  int chunk = 1024; // match xv6 BSIZE
  char *buf = (char*)malloc(chunk);
  ASSERT(buf != 0);
  memset(buf, 'A', chunk);
  for (int i = 0; i < 512; i++) { // 512KB
    int w = write(fd, buf, chunk);
    if (w != chunk) { printf("write short %d\n", w); break; }
  }
  close(fd);
  free(buf);
  uint64 large_time = get_time() - start;

  printf("[FS] Small files (%dx4B): %lu ticks\n", created, small_time);
  printf("[FS] Large file (512KB): %lu ticks\n", large_time);

  // cleanup
  for (int i = 0; i < created; i++) {
    char filename[32];
    make_name("small_", i, filename, sizeof(filename));
    unlink(filename);
  }
  unlink("large_file");
}

int main(int argc, char **argv) {
  printf("=== File System Tests ===\n");
  test_filesystem_integrity();
  test_concurrent_access();
  test_filesystem_performance();
  printf("File system tests completed.\n");
  exit(0);
}


