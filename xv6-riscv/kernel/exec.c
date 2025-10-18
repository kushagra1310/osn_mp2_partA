#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

int flags2perm(int flags)
{
  int perm = 0;
  if (flags & 0x1)
    perm = PTE_X;
  if (flags & 0x2)
    perm |= PTE_W;
  return perm;
}

int kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  // Initialize paging fields early
  p->paging.text_start = 0;
  p->paging.text_end = 0;
  p->paging.data_start = 0;
  p->paging.data_end = 0;
  p->paging.text_offset = 0;
  p->paging.data_offset = 0;
  p->paging.text_filesz = 0;
  p->paging.data_filesz = 0;

  begin_op();

  if ((ip = namei(path)) == 0)
  {
    end_op();
    return -1;
  }
  ilock(ip);

  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // SAVE the inode reference BEFORE unlocking (for lazy loading later)
  // if (p->paging.exec_ip) {
  //   iput(p->paging.exec_ip);  // Release old one if exists
  // }
  // p->paging.exec_ip = idup(ip);  // Save reference while ip is still valid
  struct inode *old_exec_ip = p->paging.exec_ip;
  if (old_exec_ip)
  {
    iput(old_exec_ip); // Within the same begin_op/end_op
  }
  p->paging.exec_ip = idup(ip);

  // Load program - lazy allocation
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
  {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;

    // Track segment info for lazy loading
    int perm = flags2perm(ph.flags);
    if (perm & PTE_X)
    {
      p->paging.text_start = ph.vaddr;
      p->paging.text_end = ph.vaddr + ph.memsz;
      p->paging.text_offset = ph.off;
      p->paging.text_filesz = ph.filesz;
    }
    else if (perm & PTE_W)
    {
      p->paging.data_start = ph.vaddr;
      p->paging.data_end = ph.vaddr + ph.memsz;
      p->paging.data_offset = ph.off;
      p->paging.data_filesz = ph.filesz;
    }

    uint64 sz1;
    if ((sz1 = uvmalloc_lazy(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
  }

  iunlockput(ip);
  end_op();
  ip = 0;

  // if (old_exec_ip)
  // {
  //   begin_op();
  //   iput(old_exec_ip);
  //   end_op();
  // }
  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate stack
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK * PGSIZE;

  p->paging.heap_start = sz;
  p->paging.stack_top = sz;

  printf("[pid %d] INIT-LAZYMAP text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap_start=0x%lx stack_top=0x%lx\n",
         p->pid, p->paging.text_start, p->paging.text_end,
         p->paging.data_start, p->paging.data_end, sz, sz);

  if (p->paging.swapfile)
  {
    fileclose(p->paging.swapfile);
    p->paging.swapfile = 0;
  }

  // Create new swap file (will overwrite same filename)
  if (create_swap_file(p) < 0)
  {
    printf("[pid %d] ERROR: Failed to create swap file\n", p->pid);
    goto bad;
  }
  // Push argument strings
  for (argc = 0; argv[argc]; argc++)
  {
    if (argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0)
    goto bad;

  p->trapframe->a1 = sp;

  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;
  p->trapframe->sp = sp;
  proc_freepagetable(oldpagetable, oldsz);

  return argc;

bad:
  if (pagetable)
    proc_freepagetable(pagetable, sz);
  if (ip)
  {
    iunlockput(ip);
    end_op();
  }
  // Clean up exec_ip if we set it
  if (p->paging.exec_ip)
  {
    begin_op();
    iput(p->paging.exec_ip);
    end_op();
    p->paging.exec_ip = 0;
  }
  return -1;
}
