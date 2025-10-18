// paging.h - Demand paging structures

#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define MAX_SWAP_PAGES 1024
#define SWAP_BITMAP_SIZE ((MAX_SWAP_PAGES + 7) / 8)
#define MAX_TRACKED_PAGES 35000

struct page_info {
  uint64 va;
  int state;        // 0=UNMAPPED, 1=RESIDENT, 2=SWAPPED
  int dirty;
  int fifo_seq;
  int swap_slot;
  uint64 exec_offset;
  int from_exec;
};

struct paging_info {
  struct file *swapfile;
  char swappath[32];
  uint8 swap_bitmap[SWAP_BITMAP_SIZE];
  int swap_used;
  int next_fifo_seq;
  struct page_info pages[MAX_TRACKED_PAGES];
  int num_pages;
  
  uint64 text_start;
  uint64 text_end;
  uint64 data_start;
  uint64 data_end;
  uint64 heap_start;
  uint64 stack_top;

  uint64 text_offset;   // File offset for text segment
  uint64 data_offset;   // File offset for data segment
  uint64 text_filesz;   // File size of text segment
  uint64 data_filesz;   // File size of data segment
  struct inode *exec_ip;
};

#endif
