// paging.h
#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define MAX_PROC_PAGES 1024

// Page info structure
struct page_info {
  uint64 va;           // Virtual address
  int state;           // UNMAPPED, RESIDENT, SWAPPED
  int is_dirty;        // Dirty flag
  int seq;             // FIFO sequence number
  int swap_slot;       // Swap slot number (-1 if not in swap)
  uint64 offset;       // Offset in executable for LOADEXEC
  uint64 filesz;       // File size for LOADEXEC
};

// Forward declaration
struct proc;

// Helper functions
struct page_info* find_page_info(struct proc *p, uint64 va);
struct page_info* alloc_page_info(struct proc *p, uint64 va);
void init_page_tracking(struct proc *p);
void free_page_tracking(struct proc *p);
struct page_info* find_fifo_victim(struct proc *p);
int evict_page(struct proc *p, struct page_info *pi);
int count_resident_pages(struct proc *p);

#endif