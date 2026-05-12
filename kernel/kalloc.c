// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)

static int refcnt[NPHYPAGES];

static int
refindex(void *pa)
{
  return ((uint64)pa - KERNBASE) / PGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    refcnt[refindex(p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int idx;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  idx = refindex(pa);
  acquire(&kmem.lock);
  if(refcnt[idx] < 1){
    release(&kmem.lock);
    panic("kfree ref");
  }
  refcnt[idx]--;
  if(refcnt[idx] > 0){
    release(&kmem.lock);
    return;
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

void
kaddref(void *pa)
{
  int idx;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kaddref");

  idx = refindex(pa);
  acquire(&kmem.lock);
  if(refcnt[idx] < 1){
    release(&kmem.lock);
    panic("kaddref ref");
  }
  refcnt[idx]++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    refcnt[refindex(r)] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

uint64
freemem_bytes(void)
{
  return (uint64)freemem_pages() * PGSIZE;
}

int
freemem_pages(void)
{
  struct run *r;
  int count = 0;

  acquire(&kmem.lock);
  for(r = kmem.freelist; r; r = r->next)
    count++;
  release(&kmem.lock);

  return count;
}
