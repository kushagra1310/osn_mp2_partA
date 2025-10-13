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

void init_page_tracking(struct proc *p)
{
  p->pages = kalloc();
  if (p->pages == 0)
    panic("init_page_tracking: kalloc");
  memset(p->pages, 0, PGSIZE);
  p->num_pages = 0;
  p->next_seq = 1;
}

void free_page_tracking(struct proc *p)
{
  if (p->pages)
  {
    kfree(p->pages);
    p->pages = 0;
  }
  p->num_pages = 0;
}

struct page_info *
find_page_info(struct proc *p, uint64 va)
{
  struct page_info *pages = (struct page_info *)p->pages;
  va = PGROUNDDOWN(va);
  for (int i = 0; i < p->num_pages; i++)
  {
    if (pages[i].va == va)
    {
      return &pages[i];
    }
  }
  return 0;
}

struct page_info *
alloc_page_info(struct proc *p, uint64 va)
{
  struct page_info *pages = (struct page_info *)p->pages;
  va = PGROUNDDOWN(va);

  // Check if already exists
  struct page_info *pi = find_page_info(p, va);
  if (pi)
    return pi;

  // Allocate new entry
  if (p->num_pages >= MAX_PROC_PAGES)
  {
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

// Find the victim page using FIFO (oldest resident page)
struct page_info *
find_fifo_victim(struct proc *p)
{
  struct page_info *pages = (struct page_info *)p->pages;
  struct page_info *victim = 0;
  int min_seq = 0x7FFFFFFF; // Max int

  for (int i = 0; i < p->num_pages; i++)
  {
    if (pages[i].state == RESIDENT && pages[i].seq < min_seq)
    {
      min_seq = pages[i].seq;
      victim = &pages[i];
    }
  }

  return victim;
}

// Evict a page (remove from page table and free physical memory)
int evict_page(struct proc *p, struct page_info *pi)
{
  pte_t *pte;
  uint64 pa;

  // Get the PTE
  pte = walk(p->pagetable, pi->va, 0);
  if (pte == 0 || (*pte & PTE_V) == 0)
  {
    return -1; // Page not mapped
  }

  // Get physical address
  pa = PTE2PA(*pte);

  // Determine if page is clean or dirty
  char *state_str = (pi->is_dirty) ? "dirty" : "clean";

  printf("[pid %d] EVICT  va=0x%lx state=%s\n", p->pid, pi->va, state_str);

  if (pi->is_dirty)
  {
    // For checkpoint 2, we'll just discard dirty pages too
    // In checkpoint 3, we'll write them to swap
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, pi->va);
  }
  else
  {
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, pi->va);
  }

  // Unmap the page
  *pte = 0;

  // Free the physical page
  kfree((void *)pa);

  // Update page info state
  pi->state = UNMAPPED;
  pi->seq = 0;

  return 0;
}

// Count resident pages for a process
int count_resident_pages(struct proc *p)
{
  struct page_info *pages = (struct page_info *)p->pages;
  int count = 0;

  for (int i = 0; i < p->num_pages; i++)
  {
    if (pages[i].state == RESIDENT)
    {
      count++;
    }
  }

  return count;
}
