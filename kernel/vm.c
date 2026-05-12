#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

struct {
  struct spinlock lock;
  struct mmap_area area[NMMAPAREA];
} mmaptable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

void
mmapinit(void)
{
  initlock(&mmaptable.lock, "mmap");
}

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

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
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
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
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
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

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
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
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

static int
pagetable_empty(pagetable_t pagetable)
{
  for(int i = 0; i < 512; i++){
    if(pagetable[i] & PTE_V)
      return 0;
  }
  return 1;
}

static int
pte_points_to_pagetable(pte_t pte)
{
  return (pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0;
}

static uint64
next_pt_boundary(uint64 va, int level)
{
  uint64 size = 1L << PXSHIFT(level);
  return (va + size) & ~(size - 1);
}

static void
free_empty_mmap_l0(pagetable_t pagetable, uint64 va)
{
  pte_t *pte2;
  pte_t *pte1;
  pagetable_t l1;
  pagetable_t l0;

  pte2 = &pagetable[PX(2, va)];
  if(!pte_points_to_pagetable(*pte2))
    return;

  l1 = (pagetable_t)PTE2PA(*pte2);
  pte1 = &l1[PX(1, va)];
  if(!pte_points_to_pagetable(*pte1))
    return;

  l0 = (pagetable_t)PTE2PA(*pte1);
  if(pagetable_empty(l0)){
    kfree((void *)l0);
    *pte1 = 0;
  }
}

static void
free_empty_mmap_l1(pagetable_t pagetable, uint64 va)
{
  pte_t *pte2;
  pagetable_t l1;

  pte2 = &pagetable[PX(2, va)];
  if(!pte_points_to_pagetable(*pte2))
    return;

  l1 = (pagetable_t)PTE2PA(*pte2);
  if(pagetable_empty(l1)){
    kfree((void *)l1);
    *pte2 = 0;
  }
}

static void
free_empty_mmap_pagetables(pagetable_t pagetable, uint64 start, uint64 length)
{
  uint64 end = start + length;

  for(uint64 va = start; va < end; ){
    uint64 next = next_pt_boundary(va, 1);
    free_empty_mmap_l0(pagetable, va);
    va = (next > va && next < end) ? next : end;
  }

  for(uint64 va = start; va < end; ){
    uint64 next = next_pt_boundary(va, 2);
    free_empty_mmap_l1(pagetable, va);
    va = (next > va && next < end) ? next : end;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
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
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
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
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if(vmfault(pagetable, va0, 0) < 0)
        return -1;
      pa0 = walkaddr(pagetable, va0);
      if(pa0 == 0)
        return -1;
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
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
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if(vmfault(pagetable, va0, 1) < 0)
        return -1;
      pa0 = walkaddr(pagetable, va0);
      if(pa0 == 0)
        return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
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
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if(vmfault(pagetable, va0, 1) < 0)
        return -1;
      pa0 = walkaddr(pagetable, va0);
      if(pa0 == 0)
        return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

static int
mmap_valid_prot(int prot)
{
  return prot == PROT_READ || prot == (PROT_READ | PROT_WRITE);
}

static int
mmap_pte_perm(int prot)
{
  int perm = PTE_R | PTE_U;

  if(prot & PROT_WRITE)
    perm |= PTE_W;
  return perm;
}

static int
mmap_lookup(struct proc *p, uint64 va, struct mmap_area *out)
{
  int found = 0;

  acquire(&mmaptable.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    struct mmap_area *area = &mmaptable.area[i];
    uint64 start = area->addr;
    if(area->p == p && va >= start && va < start + (uint64)area->length){
      *out = *area;
      found = 1;
      break;
    }
  }
  release(&mmaptable.lock);

  return found;
}

static int
mmap_take_start(struct proc *p, uint64 addr, struct mmap_area *out)
{
  int found = 0;

  acquire(&mmaptable.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    struct mmap_area *area = &mmaptable.area[i];
    if(area->p == p && area->addr == addr){
      *out = *area;
      memset(area, 0, sizeof(*area));
      found = 1;
      break;
    }
  }
  release(&mmaptable.lock);

  return found;
}

static int
mmap_take_any(struct proc *p, struct mmap_area *out)
{
  int found = 0;

  acquire(&mmaptable.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    struct mmap_area *area = &mmaptable.area[i];
    if(area->p == p){
      *out = *area;
      memset(area, 0, sizeof(*area));
      found = 1;
      break;
    }
  }
  release(&mmaptable.lock);

  return found;
}

static int
mmap_copy_index(struct proc *p, int index, struct mmap_area *out)
{
  int found = 0;

  acquire(&mmaptable.lock);
  if(index >= 0 && index < NMMAPAREA && mmaptable.area[index].p == p){
    *out = mmaptable.area[index];
    found = 1;
  }
  release(&mmaptable.lock);

  return found;
}

static int
mmap_insert(struct mmap_area *newarea)
{
  int inserted = 0;

  acquire(&mmaptable.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmaptable.area[i].p == 0){
      mmaptable.area[i] = *newarea;
      inserted = 1;
      break;
    }
  }
  release(&mmaptable.lock);

  return inserted ? 0 : -1;
}

static int
mmap_file_ok(struct file *f, int prot)
{
  if(f == 0 || f->type != FD_INODE)
    return 0;
  if((prot & PROT_READ) && f->readable == 0)
    return 0;
  if((prot & PROT_WRITE) && f->writable == 0)
    return 0;
  return 1;
}

static int
mmap_map_page(struct mmap_area *area, pagetable_t pagetable, uint64 va)
{
  uint64 mem;
  uint64 start;

  va = PGROUNDDOWN(va);
  start = area->addr;
  if(va < start || va >= start + (uint64)area->length)
    return -1;
  if(ismapped(pagetable, va))
    return -1;

  mem = (uint64)kalloc();
  if(mem == 0)
    return -1;
  memset((void *)mem, 0, PGSIZE);

  if((area->flags & MAP_ANONYMOUS) == 0){
    uint off = area->offset + (uint)(va - start);
    int n;

    if(area->f == 0 || area->f->type != FD_INODE){
      kfree((void *)mem);
      return -1;
    }

    ilock(area->f->ip);
    n = readi(area->f->ip, 0, mem, off, PGSIZE);
    iunlock(area->f->ip);
    if(n < 0){
      kfree((void *)mem);
      return -1;
    }
  }

  if(mappages(pagetable, va, PGSIZE, mem, mmap_pte_perm(area->prot)) != 0){
    kfree((void *)mem);
    return -1;
  }

  return 0;
}

uint64
kmmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
  struct proc *p = myproc();
  struct file *f = 0;
  struct mmap_area area;
  uint64 start;

  if((addr % PGSIZE) != 0 || length <= 0 || (length % PGSIZE) != 0)
    return 0;
  if(!mmap_valid_prot(prot))
    return 0;
  if(offset < 0)
    return 0;

  start = MMAPBASE + addr;

  if(flags & MAP_ANONYMOUS){
    if(fd != -1 || offset != 0)
      return 0;
  } else {
    if(fd == -1 || fd < 0 || fd >= NOFILE)
      return 0;
    f = p->ofile[fd];
    if(!mmap_file_ok(f, prot))
      return 0;
    filedup(f);
  }

  area.f = f;
  area.addr = start;
  area.length = length;
  area.offset = offset;
  area.prot = prot;
  area.flags = flags;
  area.p = p;

  if(mmap_insert(&area) < 0){
    if(f)
      fileclose(f);
    return 0;
  }

  if(flags & MAP_POPULATE){
    for(uint64 va = start; va < start + (uint64)length; va += PGSIZE){
      if(mmap_map_page(&area, p->pagetable, va) < 0)
        return 0;
    }
  }

  return start;
}

int
kmunmap(uint64 addr)
{
  struct proc *p = myproc();
  struct mmap_area area;
  uint64 start;

  if((addr % PGSIZE) != 0)
    return 0;
  if(!mmap_take_start(p, addr, &area))
    return -1;

  start = area.addr;
  uvmunmap(p->pagetable, start, area.length / PGSIZE, 1);
  free_empty_mmap_pagetables(p->pagetable, start, area.length);
  sfence_vma();
  if(area.f)
    fileclose(area.f);
  return 1;
}

void
mmapfreeproc(struct proc *p)
{
  struct mmap_area area;

  while(mmap_take_any(p, &area)){
    if(p->pagetable){
      uint64 start = area.addr;
      uvmunmap(p->pagetable, start, area.length / PGSIZE, 1);
      free_empty_mmap_pagetables(p->pagetable, start, area.length);
    }
    if(area.f)
      fileclose(area.f);
  }
  sfence_vma();
}

static int
mmap_copy_pages(struct mmap_area *area, struct proc *oldp, struct proc *newp)
{
  uint64 start = area->addr;

  for(uint64 va = start; va < start + (uint64)area->length; va += PGSIZE){
    pte_t *pte = walk(oldp->pagetable, va, 0);
    uint64 pa;
    uint flags;

    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    if((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
      continue;

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    kaddref((void *)pa);
    if(mappages(newp->pagetable, va, PGSIZE, pa, flags) != 0){
      kfree((void *)pa);
      return -1;
    }
  }

  return 0;
}

int
mmapfork(struct proc *oldp, struct proc *newp)
{
  for(int i = 0; i < NMMAPAREA; i++){
    struct mmap_area area;
    struct mmap_area child;

    if(!mmap_copy_index(oldp, i, &area))
      continue;

    child = area;
    child.p = newp;
    if(child.f)
      filedup(child.f);

    if(mmap_insert(&child) < 0){
      if(child.f)
        fileclose(child.f);
      mmapfreeproc(newp);
      return -1;
    }

    if(mmap_copy_pages(&area, oldp, newp) < 0){
      mmapfreeproc(newp);
      return -1;
    }
  }

  return 0;
}

int
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  struct mmap_area area;

  va = PGROUNDDOWN(va);

  // mmap
  if(mmap_lookup(p, va, &area)){
    if(!read && (area.prot & PROT_WRITE) == 0)
      return -1;
    if(ismapped(pagetable, va))
      return -1;
    if(mmap_map_page(&area, pagetable, va) < 0)
      return -1;
    return 1;
  }

  if(va >= MMAPBASE)
    return -1;

  // sbrk, sbrklazy
  if (va >= p->sz)
    return -1;
  if(ismapped(pagetable, va)) {
    return -1;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return -1;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return -1;
  }
  return 1;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}
