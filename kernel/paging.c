// paging.c
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "paging.h"
#include "memstat.h"

void
init_page_tracking(struct proc *p)
{
  p->pages = kalloc();
  if(p->pages == 0)
    panic("init_page_tracking: kalloc");
  memset(p->pages, 0, PGSIZE);
  p->num_pages = 0;
  p->next_seq = 1;
}

void
free_page_tracking(struct proc *p)
{
  if(p->pages) {
    kfree(p->pages);
    p->pages = 0;
  }
  p->num_pages = 0;
}

struct page_info*
find_page_info(struct proc *p, uint64 va)
{
  struct page_info *pages = (struct page_info*)p->pages;
  va = PGROUNDDOWN(va);
  for(int i = 0; i < p->num_pages; i++) {
    if(pages[i].va == va) {
      return &pages[i];
    }
  }
  return 0;
}

struct page_info*
alloc_page_info(struct proc *p, uint64 va)
{
  struct page_info *pages = (struct page_info*)p->pages;
  va = PGROUNDDOWN(va);
  
  // Check if already exists
  struct page_info *pi = find_page_info(p, va);
  if(pi)
    return pi;
  
  // Allocate new entry
  if(p->num_pages >= MAX_PROC_PAGES) {
    return 0;
  }
  
  pi = &pages[p->num_pages];
  p->num_pages++;
  
  pi->va = va;
  pi->state = UNMAPPED;
  pi->is_dirty = 0;
  pi->seq = 0;
  pi->swap_slot = -1;
  pi->offset = 0;
  pi->filesz = 0;
  
  return pi;
}