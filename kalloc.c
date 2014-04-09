// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "swap.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
pte_t* owner[MEMORYPGCAPACITY];



struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p, 0, 0);
}

//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v, int swappable,pte_t* expected_pte)
{
  struct run *r;
  uint idx = v2p(v)/PGSIZE;
  uint diskslot;

  if((uint)v % PGSIZE || v < end || v2p(v) >= PHYSTOP)
    panic("kfree2");
  if (swappable) {
    if (owner[idx] == PG_UNOWNED) {
      panic("Freeing an unowned page");
    }
    pte_t* pte = owner[v2p(v)/PGSIZE];
    if (expected_pte == pte) {
      disown(v);
      scnoderemove(v);
    }
    else {
      cprintf("PTE DIFFERS\n");
      diskslot = ((uint)*expected_pte) >> 12;
      freeswapfree(diskslot);
      return;
    }
  }
  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(int swappable)
{
  struct run *r;
  char* toevict;
  uint ondiskindex;
  if (!swappable) { //If this page is not eligible for swapping
    if(kmem.use_lock)
      acquire(&kmem.lock);
    r = kmem.freelist;
    if(r) {
      kmem.freelist = r->next;
      if (owner[v2p(r)/PGSIZE] != PG_UNOWNED) {
        panic("Alloc an owned page");
      }
    }
    if(kmem.use_lock)
      release(&kmem.lock);
    return (char*)r;
  }
  else { //If it is
    if(kmem.use_lock)
      acquire(&kmem.lock);
    r = kmem.freelist;
    if(r) {
      kmem.freelist = r->next;
      if (owner[v2p(r)/PGSIZE] != PG_UNOWNED) {
        panic("Alloc an owned page");
      }
      scnodeenqueue(r); //Page is eligible for swapping by being in the queue
    }
    else { //TODO MAKE SURE THAT THE DISK ISN'T FULL OF PAGES
      toevict = choosepageforeviction();
      ondiskindex = evict(toevict);
      *(owner[v2p(toevict)/PGSIZE]) &= (0xFFF & (~PTE_P));
      *(owner[v2p(toevict)/PGSIZE]) |= PTE_AVAIL;
      *(owner[v2p(toevict)/PGSIZE]) |= (ondiskindex<<12);
      disown(toevict);
      r = (struct run*)toevict;
    }
    if(kmem.use_lock)
      release(&kmem.lock);
    return (char*)r;
  }
}


void
own(char* va, pte_t* pte) {
  owner[v2p(va)/PGSIZE] = pte;
}

void disown(char* va) {
  owner[v2p(va)/PGSIZE] = PG_UNOWNED;
}