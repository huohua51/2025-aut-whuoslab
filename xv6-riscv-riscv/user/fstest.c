#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define BSIZE 1024

// Test large file support with indirect blocks
void test_large_file() {
  printf("\n=== Testing Large File Support ===\n");
  
  char *testfile = "largefile.txt";
  int fd = open(testfile, O_CREATE | O_RDWR);
  if(fd < 0) {
    printf("FAIL: cannot create %s\n", testfile);
    return;
  }
  
  // Write data to test indirect blocks
  // Direct blocks: 10 * 1KB = 10KB
  // Single indirect: 256 * 1KB = 256KB
  // Double indirect: 256 * 256 * 1KB = 64MB (we'll test part of it)
  
  char buf[BSIZE];
  memset(buf, 'A', BSIZE);
  
  int blocks_to_write = 300; // Test beyond single indirect (10 + 256 = 266)
  
  printf("Writing %d blocks (%d KB)...\n", blocks_to_write, blocks_to_write);
  for(int i = 0; i < blocks_to_write; i++) {
    // Mark each block with its number
    buf[0] = 'B';
    buf[1] = i & 0xFF;
    buf[2] = (i >> 8) & 0xFF;
    
    if(write(fd, buf, BSIZE) != BSIZE) {
      printf("FAIL: write error at block %d\n", i);
      close(fd);
      unlink(testfile);
      return;
    }
    
    if(i % 50 == 0 && i > 0) {
      printf("  Written %d blocks...\n", i);
    }
  }
  
  printf("Write complete. Testing read...\n");
  
  // Seek back to start
  close(fd);
  fd = open(testfile, O_RDONLY);
  
  // Read and verify some blocks
  int test_blocks[] = {0, 5, 9, 10, 11, 100, 266, 267, 268, 299};
  for(int i = 0; i < sizeof(test_blocks)/sizeof(test_blocks[0]); i++) {
    int block = test_blocks[i];
    
    // Seek to block position (simple read-based seek)
    close(fd);
    fd = open(testfile, O_RDONLY);
    for(int j = 0; j < block; j++) {
      if(read(fd, buf, BSIZE) != BSIZE) {
        printf("FAIL: seek error at block %d\n", block);
        goto cleanup;
      }
    }
    
    if(read(fd, buf, BSIZE) != BSIZE) {
      printf("FAIL: read error at block %d\n", block);
      goto cleanup;
    }
    
    if(buf[0] != 'B' || buf[1] != (block & 0xFF) || buf[2] != ((block >> 8) & 0xFF)) {
      printf("FAIL: data mismatch at block %d (got %c %d %d, expected B %d %d)\n", 
             block, buf[0], buf[1], buf[2], block & 0xFF, (block >> 8) & 0xFF);
      goto cleanup;
    }
    
    if(block == 10) {
      printf("  PASS: Direct blocks (0-9) OK\n");
    } else if(block == 266) {
      printf("  PASS: Single indirect blocks (10-265) OK\n");
    } else if(block == 268) {
      printf("  PASS: Double indirect blocks (266+) OK\n");
    }
  }
  
  printf("PASS: Large file test successful!\n");
  
cleanup:
  close(fd);
  unlink(testfile);
}

// Test symbolic link creation and reading
void test_symlink_basic() {
  printf("\n=== Testing Symbolic Links (Basic) ===\n");
  
  char *target = "/testfile.txt";
  char *linkpath = "/mylink";
  
  // Create target file
  int fd = open(target, O_CREATE | O_RDWR);
  if(fd < 0) {
    printf("FAIL: cannot create target file\n");
    return;
  }
  write(fd, "Hello, symlink!", 15);
  close(fd);
  
  // Create symlink
  if(symlink(target, linkpath) < 0) {
    printf("FAIL: symlink creation failed\n");
    unlink(target);
    return;
  }
  
  printf("Created symlink: %s -> %s\n", linkpath, target);
  
  // Read link target using readlink
  char buf[128];
  int len = readlink(linkpath, buf, sizeof(buf));
  if(len < 0) {
    printf("FAIL: readlink failed\n");
    goto cleanup_symlink;
  }
  buf[len] = '\0';
  
  if(strcmp(buf, target) != 0) {
    printf("FAIL: readlink returned wrong target: %s\n", buf);
    goto cleanup_symlink;
  }
  
  printf("PASS: readlink returned correct target: %s\n", buf);
  
  // Try to open through symlink
  fd = open(linkpath, O_RDONLY);
  if(fd < 0) {
    printf("FAIL: cannot open through symlink\n");
    goto cleanup_symlink;
  }
  
  len = read(fd, buf, sizeof(buf));
  buf[len] = '\0';
  close(fd);
  
  if(strcmp(buf, "Hello, symlink!") != 0) {
    printf("FAIL: read wrong data through symlink: %s\n", buf);
    goto cleanup_symlink;
  }
  
  printf("PASS: Successfully read through symlink\n");
  
  // Check stat type
  struct stat st;
  if(stat(linkpath, &st) < 0) {
    printf("FAIL: stat on symlink failed\n");
    goto cleanup_symlink;
  }
  
  if(st.type != T_SYMLINK) {
    printf("FAIL: symlink has wrong type: %d (expected %d)\n", st.type, T_SYMLINK);
    goto cleanup_symlink;
  }
  
  printf("PASS: Symlink has correct type\n");
  
cleanup_symlink:
  unlink(linkpath);
  unlink(target);
}

// Test symlink loop detection
void test_symlink_loop() {
  printf("\n=== Testing Symbolic Link Loop Detection ===\n");
  
  char *link1 = "/link1";
  char *link2 = "/link2";
  
  // Create circular symlinks: link1 -> link2 -> link1
  if(symlink(link2, link1) < 0) {
    printf("FAIL: cannot create link1\n");
    return;
  }
  
  if(symlink(link1, link2) < 0) {
    printf("FAIL: cannot create link2\n");
    unlink(link1);
    return;
  }
  
  printf("Created circular symlinks\n");
  
  // Try to open - should fail due to loop detection
  int fd = open(link1, O_RDONLY);
  if(fd >= 0) {
    printf("FAIL: open succeeded on circular symlink (should have failed)\n");
    close(fd);
  } else {
    printf("PASS: Loop detection prevented infinite recursion\n");
  }
  
  unlink(link1);
  unlink(link2);
}

// Test deep symlink chains
void test_symlink_chain() {
  printf("\n=== Testing Symbolic Link Chain ===\n");
  
  char *target = "/final_target.txt";
  
  // Create target file
  int fd = open(target, O_CREATE | O_RDWR);
  if(fd < 0) {
    printf("FAIL: cannot create target\n");
    return;
  }
  write(fd, "Final data", 10);
  close(fd);
  
  // Create chain: link1 -> link2 -> link3 -> target
  if(symlink(target, "/link3") < 0 ||
     symlink("/link3", "/link2") < 0 ||
     symlink("/link2", "/link1") < 0) {
    printf("FAIL: cannot create symlink chain\n");
    goto cleanup_chain;
  }
  
  printf("Created symlink chain: link1 -> link2 -> link3 -> target\n");
  
  // Try to read through the chain
  fd = open("/link1", O_RDONLY);
  if(fd < 0) {
    printf("FAIL: cannot open through symlink chain\n");
    goto cleanup_chain;
  }
  
  char buf[64];
  int len = read(fd, buf, sizeof(buf));
  buf[len] = '\0';
  close(fd);
  
  if(strcmp(buf, "Final data") != 0) {
    printf("FAIL: wrong data through chain: %s\n", buf);
    goto cleanup_chain;
  }
  
  printf("PASS: Successfully followed symlink chain\n");
  
cleanup_chain:
  unlink("/link1");
  unlink("/link2");
  unlink("/link3");
  unlink(target);
}

int main(int argc, char *argv[]) {
  printf("\n");
  printf("╔════════════════════════════════════════════╗\n");
  printf("║   xv6 File System Extended Features Test  ║\n");
  printf("╚════════════════════════════════════════════╝\n");
  
  test_large_file();
  test_symlink_basic();
  test_symlink_loop();
  test_symlink_chain();
  
  printf("\n");
  printf("╔════════════════════════════════════════════╗\n");
  printf("║          All Tests Completed!              ║\n");
  printf("╚════════════════════════════════════════════╝\n");
  printf("\n");
  
  exit(0);
}

