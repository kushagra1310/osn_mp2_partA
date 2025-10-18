#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "memstat.h"
#include "stat.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

static int alloc_swap_slot(struct proc *p)
{
  for (int i = 0; i < MAX_SWAP_PAGES; i++)
  {
    int byte_idx = i / 8;
    int bit_idx = i % 8;

    if ((p->paging.swap_bitmap[byte_idx] & (1 << bit_idx)) == 0)
    {
      p->paging.swap_bitmap[byte_idx] |= (1 << bit_idx);
      p->paging.swap_used++;
      return i;
    }
  }
  return -1;
}

static void free_swap_slot(struct proc *p, int slot)
{
  if (slot < 0 || slot >= MAX_SWAP_PAGES)
    return;

  int byte_idx = slot / 8;
  int bit_idx = slot % 8;

  if (p->paging.swap_bitmap[byte_idx] & (1 << bit_idx))
  {
    p->paging.swap_bitmap[byte_idx] &= ~(1 << bit_idx);
    p->paging.swap_used--;
  }
}

int create_swap_file(struct proc *p)
{
  if (p->paging.swapfile != 0)
    return 0;

  char name[16];
  name[0] = 'p';
  name[1] = 'g';
  name[2] = 's';
  name[3] = 'w';
  name[4] = 'p';

  int pid = p->pid;
  for (int i = 9; i >= 5; i--)
  {
    name[i] = '0' + (pid % 10);
    pid /= 10;
  }
  name[10] = '\0';

  safestrcpy(p->paging.swappath, name, sizeof(p->paging.swappath));

  begin_op();
  struct inode *ip = create(name, T_FILE, 0, 0);
  if (ip == 0)
  {
    end_op();
    return -1;
  }
  iunlock(ip);

  struct file *f = filealloc();
  if (f == 0)
  {
    begin_op();
    ilock(ip);
    ip->nlink = 0;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    end_op();
    return -1;
  }

  f->type = FD_INODE;
  f->off = 0;
  f->readable = 1;
  f->writable = 1;
  f->ip = ip;

  p->paging.swapfile = f;
  end_op();
  return 0;
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;)
  {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    if (do_free)
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);

    pa0 = walkaddr(pagetable, va0);

    if (pa0 == 0)
    {
      pa0 = handle_kernel_pagefault(pagetable, va0, 1);
      if (pa0 == 0)
        return -1;
    }

    // CRITICAL: Check writability
    pte_t *pte = walk(pagetable, va0, 0);
    if (!pte)
    {
      // printf("copyout: no PTE for va=0x%lx\n", va0);
      return -1;
    }

    // printf("copyout: va=0x%lx pte=0x%lx PTE_V=%d PTE_W=%d\n",
    //  va0, *pte, (*pte & PTE_V) ? 1 : 0, (*pte & PTE_W) ? 1 : 0);

    if (!(*pte & PTE_W))
    {
      // printf("copyout: REJECT write to read-only page va=0x%lx\n", va0);
      return -1;
    }

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);

    // If page not present, try to handle page fault
    if (pa0 == 0)
    {
      pa0 = handle_kernel_pagefault(pagetable, va0, 0);
      if (pa0 == 0)
        return -1;
    }

    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);

    // If page not present, try to handle page fault
    if (pa0 == 0)
    {
      pa0 = handle_kernel_pagefault(pagetable, va0, 0);
      if (pa0 == 0)
        return -1;
    }

    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)pa0 + (srcva - va0);
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }

  if (got_null)
    return 0;
  else
    return -1;
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pagetable, va))
  {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0)
  {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
  {
    return 0;
  }
  if (*pte & PTE_V)
  {
    return 1;
  }
  return 0;
}
// Handle page fault from kernel context (e.g., copyout/copyin)
// Returns physical address if successful, 0 otherwise
// Handle page fault from kernel context (e.g., copyout/copyin)
// Returns physical address if successful, 0 otherwise
uint64 handle_kernel_pagefault(pagetable_t pagetable, uint64 va, int is_write)
{
  va = PGROUNDDOWN(va);

  // Trigger lazy load with appropriate access type
  int cause = is_write ? 15 : 13; // 15=write, 13=read
  if (handle_page_fault(va, cause) == 0)
  {
    uint64 pa = walkaddr(pagetable, va);
    if (pa == 0)
      return 0;

    // If write access is needed, check writability
    if (is_write)
    {
      pte_t *pte = walk(pagetable, va, 0);
      if (!pte || !(*pte & PTE_W))
      {
        return 0; // Not writable - reject
      }
    }

    return pa; // Success
  }
  return 0;
}

void init_paging_info(struct proc *p)
{
  memset(&p->paging, 0, sizeof(struct paging_info));
  p->paging.next_fifo_seq = 1;
  p->paging.swapfile = 0;
  p->paging.swap_used = 0;
  p->paging.exec_ip = 0;

  for (int i = 0; i < 512; i++)
  {
    p->paging.pages[i].state = UNMAPPED;
    p->paging.pages[i].swap_slot = -1;
    p->paging.pages[i].fifo_seq = 0;
    p->paging.pages[i].from_exec = 0;
  }

  memset(p->paging.swap_bitmap, 0, SWAP_BITMAP_SIZE);
}

void cleanup_paging_info(struct proc *p)
{
  // Just print stats - don't actually close files here
  // The file cleanup will happen through normal process cleanup
  // if(p->paging.swapfile) {
  //   printf("[pid %d] SWAPCLEANUP freed_slots=%d\n", p->pid, p->paging.swap_used);
  //   // Don't call fileclose here - causes lock issues
  //   // fileclose(p->paging.swapfile);
  //   p->paging.swapfile = 0;
  // }

  // // Don't free exec_ip here either
  // p->paging.exec_ip = 0;
}

struct page_info *get_page_info(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  uint64 page_num = va / PGSIZE;
  if (page_num >= MAX_TRACKED_PAGES)
    return 0;
  return &p->paging.pages[page_num];
}

int handle_page_fault(uint64 va, int cause)
{
  struct proc *p = myproc();
  va = PGROUNDDOWN(va);

  //  if(va == 0x0) {
  //   printf("[pid %d] DEBUG: va=0x0 text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap=[0x%lx,0x%lx)\n",
  //          p->pid, p->paging.text_start, p->paging.text_end,
  //          p->paging.data_start, p->paging.data_end,
  //          p->paging.heap_start, p->sz);
  // }
  char *access_type;
  if (cause == 13)
    access_type = "read";
  else if (cause == 15)
    access_type = "write";
  else
    access_type = "exec";
  if ((va >= p->paging.text_start && va < p->paging.text_end) && cause == 15)
  {
    setkilled(p);
    return 0;
  }
  // pte_t *pte = walk(p->pagetable, va, 0);
  // if(pte && (*pte & PTE_V)) {
  //   // Page is already valid - this shouldn't happen
  //   printf("[pid %d] WARNING: page fault on already-mapped page va=0x%lx\n", p->pid, va);
  //   return 0;  // Already handled
  // }

  // Check if page is swapped FIRST (before anything else)
  struct page_info *pi = get_page_info(p, va);
  if (pi && pi->state == SWAPPED)
  {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=swap\n",
           p->pid, va, access_type);
    return swapin_page(va);
  }

  // Check if in text segment
  if (va >= p->paging.text_start && va < p->paging.text_end)
  {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n",
           p->pid, va, access_type);

    char *mem = kalloc();
    if (mem == 0)
    {
      printf("[pid %d] MEMFULL\n", p->pid);
      if (evict_page_fifo() < 0)
      {
        printf("[pid %d] KILL out-of-memory\n", p->pid);
        return -1;
      }
      mem = kalloc();
      if (mem == 0)
      {
        printf("[pid %d] KILL out-of-memory\n", p->pid);
        return -1;
      }
    }

    memset(mem, 0, PGSIZE);

    // Calculate file offset for this page
    uint64 page_offset_in_segment = va - p->paging.text_start;
    uint64 file_offset = p->paging.text_offset + page_offset_in_segment;
    uint64 read_size = PGSIZE;

    // Don't read beyond file size
    if (page_offset_in_segment >= p->paging.text_filesz)
    {
      read_size = 0;
    }
    else if (page_offset_in_segment + PGSIZE > p->paging.text_filesz)
    {
      read_size = p->paging.text_filesz - page_offset_in_segment;
    }

    if (read_size > 0 && p->paging.exec_ip)
    {
      begin_op();
      ilock(p->paging.exec_ip);
      if (readi(p->paging.exec_ip, 0, (uint64)mem, file_offset, read_size) != read_size)
      {
        iunlock(p->paging.exec_ip);
        end_op();
        kfree(mem);
        printf("[pid %d] KILL read-failed\n", p->pid);
        return -1;
      }
      iunlock(p->paging.exec_ip);
      end_op();
    }

    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_X) < 0)
    {
      kfree(mem);
      printf("[pid %d] KILL mapping-failed\n", p->pid);
      return -1;
    }

    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);

    if (pi)
    {
      pi->va = va;
      pi->state = RESIDENT;
      pi->dirty = 0;
      pi->fifo_seq = p->paging.next_fifo_seq++;
      pi->from_exec = 1;
      printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi->fifo_seq);
    }

    return 0;
  }

  // Check if in data segment
  if (va >= p->paging.data_start && va < p->paging.data_end)
  {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n",
           p->pid, va, access_type);

    char *mem = kalloc();
    if (mem == 0)
    {
      printf("[pid %d] MEMFULL\n", p->pid);
      if (evict_page_fifo() < 0)
      {
        printf("[pid %d] KILL out-of-memory\n", p->pid);
        return -1;
      }
      mem = kalloc();
      if (mem == 0)
      {
        printf("[pid %d] KILL out-of-memory\n", p->pid);
        return -1;
      }
    }

    memset(mem, 0, PGSIZE);

    uint64 page_offset_in_segment = va - p->paging.data_start;
    uint64 file_offset = p->paging.data_offset + page_offset_in_segment;
    uint64 read_size = PGSIZE;

    if (page_offset_in_segment >= p->paging.data_filesz)
    {
      read_size = 0;
    }
    else if (page_offset_in_segment + PGSIZE > p->paging.data_filesz)
    {
      read_size = p->paging.data_filesz - page_offset_in_segment;
    }

    if (read_size > 0 && p->paging.exec_ip)
    {
      begin_op();
      ilock(p->paging.exec_ip);
      if (readi(p->paging.exec_ip, 0, (uint64)mem, file_offset, read_size) != read_size)
      {
        iunlock(p->paging.exec_ip);
        end_op();
        kfree(mem);
        printf("[pid %d] KILL read-failed\n", p->pid);
        return -1;
      }
      iunlock(p->paging.exec_ip);
      end_op();
    }

    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) < 0)
    {
      kfree(mem);
      printf("[pid %d] KILL mapping-failed\n", p->pid);
      return -1;
    }

    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);

    if (pi)
    {
      pi->va = va;
      pi->state = RESIDENT;
      pi->dirty = 0;
      pi->fifo_seq = p->paging.next_fifo_seq++;
      pi->from_exec = 1;
      printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi->fifo_seq);
    }

    return 0;
  }

  // Heap
  if (va >= p->paging.heap_start && va < p->sz)
  {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=heap\n",
           p->pid, va, access_type);

    char *mem = kalloc();
    if (mem == 0)
    {
      printf("[pid %d] MEMFULL\n", p->pid);
      if (evict_page_fifo() < 0)
      {
        printf("[pid %d] KILL out-of-memory\n", p->pid);
        return -1;
      }
      mem = kalloc();
      if (mem == 0)
      {
        printf("[pid %d] KILL out-of-memory\n", p->pid);
        return -1;
      }
    }

    memset(mem, 0, PGSIZE);

    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) < 0)
    {
      kfree(mem);
      printf("[pid %d] KILL mapping-failed\n", p->pid);
      return -1;
    }

    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);

    if (pi)
    {
      pi->va = va;
      pi->state = RESIDENT;
      pi->dirty = 1; // Mark heap pages as dirty
      pi->fifo_seq = p->paging.next_fifo_seq++;
      pi->from_exec = 0;
      pi->swap_slot = -1;

      printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi->fifo_seq);
    }

    return 0;
  }

  // Invalid
  printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=invalid\n",
         p->pid, va, access_type);
  printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n",
         p->pid, va, access_type);

  return -1;
}

uint64 alloc_zero_page(pagetable_t pagetable, uint64 va, int perm)
{
  return 0; // Not implemented yet
}

uint64 load_exec_page(pagetable_t pagetable, uint64 va, struct inode *ip,
                      uint64 offset, uint sz, int perm)
{
  return 0; // Not implemented yet
}

int evict_page_fifo(void)
{
  struct proc *p = myproc();

  int victim_idx = -1;
  int min_seq = 0x7FFFFFFF;

  for (int i = 0; i < 35000; i++)
  {
    if (p->paging.pages[i].state == RESIDENT)
    {
      if (p->paging.pages[i].fifo_seq < min_seq)
      {
        min_seq = p->paging.pages[i].fifo_seq;
        victim_idx = i;
      }
    }
  }

  if (victim_idx < 0)
    return -1;

  uint64 victim_va = victim_idx * PGSIZE;

  printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n",
         p->pid, victim_va, min_seq);

  return swapout_page(victim_va);
}

int swapin_page(uint64 va)
{
  struct proc *p = myproc();
  va = PGROUNDDOWN(va);

  struct page_info *pi = get_page_info(p, va);
  if (!pi || pi->state != SWAPPED || pi->swap_slot < 0)
    return -1;

  int slot = pi->swap_slot;

  char *mem = kalloc();
  if (mem == 0)
  {
    printf("[pid %d] MEMFULL\n", p->pid);
    if (evict_page_fifo() < 0)
      return -1;
    mem = kalloc();
    if (mem == 0)
      return -1;
  }

  begin_op();
  ilock(p->paging.swapfile->ip);
  uint64 offset = (uint64)slot * PGSIZE;
  if (readi(p->paging.swapfile->ip, 0, (uint64)mem, offset, PGSIZE) != PGSIZE)
  {
    iunlock(p->paging.swapfile->ip);
    end_op();
    kfree(mem);
    return -1;
  }
  iunlock(p->paging.swapfile->ip);
  end_op();

  printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, va, slot);

  free_swap_slot(p, slot);
  pi->swap_slot = -1;

  int perm = PTE_U | PTE_R | PTE_W;
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0)
  {
    kfree(mem);
    return -1;
  }

  pi->state = RESIDENT;
  pi->dirty = 0;
  pi->fifo_seq = p->paging.next_fifo_seq++;

  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, pi->fifo_seq);

  return 0;
}

int swapout_page(uint64 va)
{
  struct proc *p = myproc();
  va = PGROUNDDOWN(va);

  struct page_info *pi = get_page_info(p, va);
  if (!pi || pi->state != RESIDENT)
    return -1;

  // Check if clean page from exec
  if (!pi->dirty && pi->from_exec)
  {
    printf("[pid %d] EVICT  va=0x%lx state=clean\n", p->pid, va);
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, va);

    pi->state = UNMAPPED;

    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V))
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
      *pte = 0;
    }

    return 0;
  }

  // Dirty page - must swap
  printf("[pid %d] EVICT  va=0x%lx state=dirty\n", p->pid, va);

  if (p->paging.swapfile == 0)
  {
    if (create_swap_file(p) < 0)
    {
      printf("[pid %d] SWAPFULL\n", p->pid);
      printf("[pid %d] KILL swap-exhausted\n", p->pid);
      setkilled(p);
      return -1;
    }
  }

  int slot = alloc_swap_slot(p);
  if (slot < 0)
  {
    printf("[pid %d] SWAPFULL\n", p->pid);
    printf("[pid %d] KILL swap-exhausted\n", p->pid);
    setkilled(p);
    return -1;
  }

  pte_t *pte = walk(p->pagetable, va, 0);
  if (!pte || !(*pte & PTE_V))
  {
    free_swap_slot(p, slot);
    return -1;
  }

  uint64 pa = PTE2PA(*pte);

  begin_op();
  ilock(p->paging.swapfile->ip);
  uint64 offset = (uint64)slot * PGSIZE;
  if (writei(p->paging.swapfile->ip, 0, pa, offset, PGSIZE) != PGSIZE)
  {
    iunlock(p->paging.swapfile->ip);
    end_op();
    free_swap_slot(p, slot);
    return -1;
  }
  iunlock(p->paging.swapfile->ip);
  end_op();

  printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, va, slot);

  pi->state = SWAPPED;
  pi->swap_slot = slot;
  pi->from_exec = 0;

  kfree((void *)pa);
  *pte = 0;

  return 0;
}

uint64 uvmalloc_lazy(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int perm)
{
  // Don't create any PTEs - just return the new size
  // Pages will be allocated on-demand via page faults
  return newsz;
}
