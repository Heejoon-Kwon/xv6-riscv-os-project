// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

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

struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

struct spinlock lru_lock;

struct {
  struct spinlock lock;
  uchar *bitmap;
} swapspace;

#define BLKS_PER_PG (PGSIZE / BSIZE)
#define NSWAPPG (SWAPMAX / BLKS_PER_PG)

static struct page*
pa2page(uint64 pa)
{
  if(pa >= PHYSTOP)
    panic("pa2page");
  return &pages[pa / PGSIZE];
}

static uint64
page2pa(struct page *page)
{
  return ((uint64)(page - pages)) * PGSIZE;
}

static int
page_on_lru(struct page *page)
{
  return page->next != 0;
}

static void
lru_remove_locked(struct page *page)
{
  if(!page_on_lru(page))
    return;

  if(page->next == page){
    page_lru_head = 0;
  } else {
    page->prev->next = page->next;
    page->next->prev = page->prev;
    if(page_lru_head == page)
      page_lru_head = page->next;
  }

  page->next = 0;
  page->prev = 0;
  page->pagetable = 0;
  page->vaddr = 0;
  num_lru_pages--;
}

static int
swap_slot_alloc(void)
{
  int slot;

  acquire(&swapspace.lock);
  for(slot = 0; slot < NSWAPPG; slot++){
    uchar mask = 1 << (slot % 8);
    if((swapspace.bitmap[slot / 8] & mask) == 0){
      swapspace.bitmap[slot / 8] |= mask;
      release(&swapspace.lock);
      return slot;
    }
  }
  release(&swapspace.lock);
  return -1;
}

void
swap_slot_free(int slot)
{
  uchar mask;

  if(slot < 0 || slot >= NSWAPPG)
    panic("swap_slot_free");

  mask = 1 << (slot % 8);
  acquire(&swapspace.lock);
  if((swapspace.bitmap[slot / 8] & mask) == 0)
    panic("swap_slot_free: free");
  swapspace.bitmap[slot / 8] &= ~mask;
  release(&swapspace.lock);
}

static void*
swapout(void)
{
  struct page *page;
  pagetable_t pagetable;
  uint64 va, pa;
  pte_t *pte;
  uint flags;
  int slot;

  for(;;){
    acquire(&lru_lock);
    if(page_lru_head == 0){
      release(&lru_lock);
      return 0;
    }

    page = page_lru_head;
    pagetable = page->pagetable;
    va = (uint64)page->vaddr;
    pa = page2pa(page);
    pte = walk(pagetable, va, 0);

    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
       PTE2PA(*pte) != pa){
      lru_remove_locked(page);
      release(&lru_lock);
      continue;
    }

    if(*pte & PTE_A){
      *pte &= ~PTE_A;
      page_lru_head = page->next;
      sfence_vma();
      release(&lru_lock);
      continue;
    }

    slot = swap_slot_alloc();
    if(slot < 0){
      release(&lru_lock);
      return 0;
    }

    flags = PTE_FLAGS(*pte) & ~PTE_V;
    *pte = PA2PTE((uint64)slot << PGSHIFT) | flags;
    sfence_vma();
    lru_remove_locked(page);
    release(&lru_lock);

    swapwrite(pa, slot);
    return (void*)pa;
  }
}

void
page_lru_add(pagetable_t pagetable, uint64 va, uint64 pa)
{
  struct page *page;

  page = pa2page(pa);
  acquire(&lru_lock);
  if(page_on_lru(page))
    lru_remove_locked(page);

  page->pagetable = pagetable;
  page->vaddr = (char*)va;
  if(page_lru_head == 0){
    page->next = page;
    page->prev = page;
    page_lru_head = page;
  } else {
    struct page *tail = page_lru_head->prev;
    page->next = page_lru_head;
    page->prev = tail;
    tail->next = page;
    page_lru_head->prev = page;
  }
  num_lru_pages++;
  release(&lru_lock);
}

void
page_lru_remove(uint64 pa)
{
  acquire(&lru_lock);
  lru_remove_locked(pa2page(pa));
  release(&lru_lock);
}

int
swapin(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint flags;
  int slot;
  char *mem;

  va = PGROUNDDOWN(va);
  pte = walk(pagetable, va, 0);
  if(pte == 0 || !PTE_SWAPPED(*pte))
    return -1;

  slot = PTE2PA(*pte) >> PGSHIFT;
  flags = PTE_FLAGS(*pte);

  if((mem = kalloc()) == 0)
    return -1;

  swapread((uint64)mem, slot);
  swap_slot_free(slot);
  *pte = PA2PTE((uint64)mem) | flags | PTE_V;
  sfence_vma();
  page_lru_add(pagetable, va, (uint64)mem);

  return 0;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&lru_lock, "lru");
  initlock(&swapspace.lock, "swapspace");
  page_lru_head = 0;
  num_free_pages = 0;
  num_lru_pages = 0;
  freerange(end, (void*)PHYSTOP);
  swapspace.bitmap = kalloc();
  if(swapspace.bitmap == 0)
    panic("swap bitmap");
  memset(swapspace.bitmap, 0, PGSIZE);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  num_free_pages++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
// pa4: kalloc function
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    num_free_pages--;
  }
  release(&kmem.lock);

  if(r == 0)
    r = swapout();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  else
    printf("kalloc: out of memory\n");
  return (void*)r;
}