#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "paging.h"
#include "memstat.h"

static int loadseg_lazy(struct proc *p, uint64 va, struct inode *ip, uint offset, uint filesz, uint memsz);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Initialize page tracking
  init_page_tracking(p);
  
  uint64 text_start = 0xFFFFFFFFFFFFFFFF;
  uint64 text_end = 0;
  uint64 data_start = 0xFFFFFFFFFFFFFFFF;
  uint64 data_end = 0;

  // Load program segments LAZILY - don't allocate or load pages yet
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    
    // Track text/data regions
    int perm = flags2perm(ph.flags);
    if(perm & PTE_X) {
      // Text segment
      if(ph.vaddr < text_start) text_start = ph.vaddr;
      if(ph.vaddr + ph.memsz > text_end) text_end = ph.vaddr + ph.memsz;
    } else {
      // Data segment
      if(ph.vaddr < data_start) data_start = ph.vaddr;
      if(ph.vaddr + ph.memsz > data_end) data_end = ph.vaddr + ph.memsz;
    }
    
    // Register pages for lazy loading
    if(loadseg_lazy(p, ph.vaddr, ip, ph.off, ph.filesz, ph.memsz) < 0)
      goto bad;
    
    if(ph.vaddr + ph.memsz > sz)
      sz = ph.vaddr + ph.memsz;
  }
  
  // Keep inode reference for lazy loading
  p->exec_ip = idup(ip);
  
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate stack pages LAZILY
  sz = PGROUNDUP(sz);
  uint64 sz1 = sz + (USERSTACK+1)*PGSIZE;
  
  // Don't actually allocate - just update size
  sz = sz1;
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Store segment boundaries
  p->text_start = text_start;
  p->text_end = text_end;
  p->data_start = (data_start == 0xFFFFFFFFFFFFFFFF) ? text_end : data_start;
  p->data_end = (data_end == 0) ? p->data_start : data_end;
  p->heap_start = PGROUNDUP(p->data_end);
  p->stack_top = sz;
  p->next_seq=1;

  //  printf("[pid %d] EXEC: next_seq initialized to %d\n", p->pid, p->next_seq);

  // Log initialization
  printf("[pid %d] INIT-LAZYMAP text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap_start=0x%lx stack_top=0x%lx\n",
         p->pid, p->text_start, p->text_end, p->data_start, p->data_end, 
         p->heap_start, p->stack_top);

  // Copy argument strings into new stack
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  p->trapframe->a1 = sp;

  // Save program name
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;
  p->trapframe->sp = sp;
  
  // Force load the entry point page so execution can start
  // This is done so that os doesn't immediately go back to kernel mode after entering the user mode at the time of starting of a process. This ensures no errors (was getting some without using it)
  
  uint64 entry_va = PGROUNDDOWN(elf.entry);
  struct page_info *entry_pi = find_page_info(p, entry_va);
  
  if(entry_pi && entry_pi->state == UNMAPPED) {
    char *mem = kalloc();
    if(mem == 0) {
      proc_freepagetable(pagetable, sz);
      p->pagetable = oldpagetable;
      p->sz = oldsz;
      return -1;
    }
    memset(mem, 0, PGSIZE);
    
    // Load from executable
    if(entry_pi->filesz > 0) {
      if(readi(p->exec_ip, 0, (uint64)mem, entry_pi->offset, entry_pi->filesz) != entry_pi->filesz) {
        kfree(mem);
        proc_freepagetable(pagetable, sz);
        p->pagetable = oldpagetable;
        p->sz = oldsz;
        return -1;
      }
    }
    
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, entry_va);

    // Map with execute permission for text
    int perm = PTE_U | PTE_R | PTE_X;
    if(mappages(pagetable, entry_va, PGSIZE, (uint64)mem, perm) != 0) {
      kfree(mem);
      proc_freepagetable(pagetable, sz);
      p->pagetable = oldpagetable;
      p->sz = oldsz;
      return -1;
    }
    
    // Update page info
    entry_pi->state = RESIDENT;
    entry_pi->is_dirty = 0;
    entry_pi->seq = p->next_seq++;
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, entry_va, entry_pi->seq); // we map it and store it without pagefault
  }
  
  proc_freepagetable(oldpagetable, oldsz);

  return argc;

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Register pages for lazy loading without allocating them
static int
loadseg_lazy(struct proc *p, uint64 va, struct inode *ip, uint offset, uint filesz, uint memsz)
{
  uint64 i;
  
  for(i = 0; i < memsz; i += PGSIZE){
    struct page_info *pi = alloc_page_info(p, va + i);
    if(pi == 0)
      return -1;
    
    pi->state = UNMAPPED;
    pi->offset = offset + i;
    
    // Calculate file size for this page
    if(i < filesz) {
      if(filesz - i < PGSIZE)
        pi->filesz = filesz - i;
      else
        pi->filesz = PGSIZE;
    } else {
      pi->filesz = 0; // BSS segment
    }
  }
  
  return 0;
}