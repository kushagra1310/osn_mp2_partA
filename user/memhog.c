#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Simple memory test to trigger page faults and replacement
int
main(int argc, char *argv[])
{
  int pages = 1123;  // Try to allocate 20 pages (80 KB)
  
  if(argc > 1) {
    pages = atoi(argv[1]);
  }
  
  printf("memtest: allocating %d pages\n", pages);
  
  char *mem = sbrk(pages * 4096);
  if(mem == (char*)-1) {
    printf("memtest: sbrk failed\n");
    exit(1);
  }
  
  printf("memtest: writing to pages\n");
  
  // Write to each page to trigger page faults
  for(int i = 0; i < pages; i++) {
    // Write to the first byte of each page
    mem[i * 4096] = 'A' + (i % 26);
    if(i % 5 == 0) {
      printf("memtest: wrote to page %d\n", i);
    }
  }
  
  printf("memtest: reading back\n");
  
  // Read back to verify
  for(int i = 0; i < pages; i++) {
    char val = mem[i * 4096];
    if(val != 'A' + (i % 26)) {
      printf("memtest: ERROR at page %d: expected %c, got %c\n", 
             i, 'A' + (i % 26), val);
      exit(1);
    }
    if(i % 5 == 0) {
      printf("memtest: verified page %d\n", i);
    }
  }
  
  printf("memtest: SUCCESS - all pages verified\n");
  exit(0);
}