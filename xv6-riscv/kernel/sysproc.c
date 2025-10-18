#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "memstat.h"  

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  // int addr;
  int n;
  uint64 old_sz;
  struct proc *p = myproc();

  argint(0, &n);
  
  old_sz = p->sz;
  
  if(n > 0) {
    // Growing heap - use lazy allocation (just update size)
    p->sz = old_sz + n;
  } else if(n < 0) {
    // Shrinking heap - actually free pages
    p->sz = uvmdealloc(p->pagetable, old_sz, old_sz + n);
  }
  
  return old_sz;
}


uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
uint64
sys_memstat(void)
{
  uint64 addr;
  
  argaddr(0, &addr);
  
  struct proc *p = myproc();
  
  // Allocate kernel buffer
  struct proc_mem_stat *info = (struct proc_mem_stat*)kalloc();
  if(info == 0) {
    return -1;
  }
  
  // IMPORTANT: Zero out the buffer first
  memset(info, 0, sizeof(struct proc_mem_stat));
  
  // Fill basic info
  info->pid = p->pid;
  info->next_fifo_seq = p->paging.next_fifo_seq;
  info->num_pages_total = 0;
  info->num_resident_pages = 0;
  info->num_swapped_pages = 0;
  
  // Count pages
  int page_count = 0;
  for(int i = 0; i < MAX_TRACKED_PAGES && page_count < MAX_PAGES_INFO; i++) {
    if(p->paging.pages[i].state != UNMAPPED) {
      info->pages[page_count].va = p->paging.pages[i].va;
      info->pages[page_count].state = p->paging.pages[i].state;
      info->pages[page_count].is_dirty = p->paging.pages[i].dirty;
      info->pages[page_count].seq = p->paging.pages[i].fifo_seq;
      info->pages[page_count].swap_slot = p->paging.pages[i].swap_slot;
      
      if(p->paging.pages[i].state == RESIDENT)
        info->num_resident_pages++;
      else if(p->paging.pages[i].state == SWAPPED)
        info->num_swapped_pages++;
        
      page_count++;
    }
  }
  
  info->num_pages_total = page_count;
  
  // Copy to user space
  int result = copyout(p->pagetable, addr, (char *)info, sizeof(struct proc_mem_stat));
  
  kfree((void*)info);
  
  if(result < 0)
    return -1;
    
  return 0;
}
