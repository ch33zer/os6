#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "fs.h"
#include "x86.h"
#include "proc.h"
#include "swap.h"
#include "spinlock.h"

/*  Sentinel nodes pointing to beginning and end of the
		second chance queue of evict-candidate pages. */
static struct scnode schead;
static struct scnode sctail;
static struct scnode nodememory[MEMORYPGCAPACITY];
static struct spinlock sclock;

/*  Parallel to kmem.freelist for keeping track of free
		pages on the swap drive */
static struct freeswapnode* swaphead;
static struct freeswapnode swapmemory[SWAPPGCAPACITY];
static struct spinlock freeswaplock;

void
swapinit() {
	// Setup disk free structure
	struct freeswapnode* curr;
	struct freeswapnode* next;
	int i;
	swaphead = &(swapmemory[0]);
	curr = swaphead;
	for (i = 1; i < SWAPPGCAPACITY; i++) {
		curr->index = i - 1;
		next = &(swapmemory[i]);
		curr->next = next;
		curr = next;
	}
	curr->next = 0;
	curr->index = i-1;

	// Setup second chance queue
	schead.next = &sctail;
	sctail.prev = &schead;

	// Initialize locks
	initlock(&sclock, "scqueue");
	initlock(&freeswaplock,"freeswap");
	

}

// isreferenced and setunreferenced are called
// with sclock held in choosepageforeviction.
//	Check if accessed
int isreferenced(uint idx) {
	return *(owner[idx]) & PTE_A;
}

//	Unset accessed bit.
void setunreferenced(uint idx) {
	*(owner[idx]) &= ~(PTE_A);
}

//	Check scqueue (second chance queue) for a page to evict.
char*
choosepageforeviction(void) {
	acquire(&sclock);
	struct scnode* curr = schead.next;
	if (!curr || curr == &sctail) {
		panic("no pages to evict!");
	}
	while (isreferenced(curr->index)) {
		setunreferenced(curr->index);
		//unlink from head
		schead.next = curr->next;
		curr->next->prev = &schead;
		//link to end
		(sctail.prev)->next = curr;
		curr->next = &sctail;
		curr->prev = sctail.prev;
		sctail.prev = curr;
		curr = schead.next;
	}

	schead.next = curr->next;
	curr->next->prev = &schead;

	curr->next = 0;
	curr->prev = 0;
	release(&sclock);
	//cprintf("Evict 0x%x\n",owner[curr->index]);
	return p2v(curr->index * PGSIZE); // Returns address in kernel space
}

//	Adds evict candidates to back of queue.
void
scnodeenqueue(void* va) {

	acquire(&sclock);
	uint idx = v2p(va)/PGSIZE;
	if (idx <0 || idx >= MEMORYPGCAPACITY) {
		panic("scnodenequeue invalid slot idx");
	}
	struct scnode* slot = &(nodememory[idx]);
	if (slot->next || slot->prev) {
		panic("scnodeenqueue of existing page");
	}
	if (slot->index != 0 && idx != slot->index) {
		panic("scnodeenqueue invalid slot->index");
	}
	slot->index = idx;
	(sctail.prev)->next = slot;
	slot->next = &sctail;
	slot->prev = sctail.prev;
	sctail.prev = slot;
	release(&sclock);
}

/*
 Given an evictable page, remove it from the queue.
 Used when kfree is called, so that pages are not
 evicted after being freed.
*/
void
scnoderemove(void* va) {
	acquire(&sclock);
	uint idx = v2p(va)/PGSIZE;
	if (idx <0 || idx >= MEMORYPGCAPACITY) {
		panic("scnodenequeue invalid slot idx");
	}
	struct scnode* slot = &(nodememory[idx]);
	struct scnode* prev = slot->prev;
	struct scnode* next = slot->next;
	if (!next || !prev) {
		panic("scnoderemove of non present node");
	}
	prev->next = next;
	next->prev = prev;
	slot->next = 0;
	slot->prev = 0;
	release(&sclock);
}

// Add node at index back to the freelist.
void
freeswapfree(uint index) {
	acquire(&freeswaplock);
	struct freeswapnode* node;
	node = &(swapmemory[index]);
	if (index < 0 || index >= SWAPPGCAPACITY) {
		panic("Invalid swap index");
	}
	if (node->next) {
		panic("freeswapfree of already free node");
	}
	node->next = swaphead;
	swaphead = node;
	
	release(&freeswaplock);
}

// Grab a free spot in swap.
struct freeswapnode*
getfreenode() {
	acquire(&freeswaplock);
	if (!swaphead) {
		//Out of swap pages
		release(&freeswaplock);
		return 0;
	}
	
	struct freeswapnode* node = swaphead;
	swaphead = swaphead->next;
	node->next = 0;
	release(&freeswaplock);
	return node;
}

/*
	Check if the page is in memory, 
	Else read it back to memory.

	Ownerlock should be called. 
*/
int 
unswappage(pte_t* pte) {
	if (!PTE_ONDISK(*pte)) {
		return 1;
	}
	char* newmem = kalloc(1);
	if (!newmem) {
		return 0;
	}
	uint diskidx = ((uint)*pte) >> 12;
	uint flags = ((uint)*pte) & 0xFFF;
	flags |= PTE_P;
	flags &= ~PTE_AVAIL;
	readpg(newmem, diskidx * PGSIZE / BSIZE);
	*pte = flags | v2p(newmem);
	own(newmem, pte);
	return 1;
}

/*
	Get a free page in memory, evict if necessary. 
*/
char*
swappage(void) {
	char* toevict = choosepageforeviction();
	if (!toevict) {
		return 0;
	}
	struct freeswapnode* node = getfreenode();
	if (!node) {
		scnodeenqueue(toevict); // The page is still in memory
		return 0;
	}
	//cprintf("Evicting page %p!\n",toevict);
	uint ondiskindex = node->index;
	uint oidx =v2p(toevict)/PGSIZE;
	pte_t* pte = owner[oidx];
	if (pte == PG_UNOWNED) {
		panic("Eviction of unowned page!");
	}
	writepg(toevict, ondiskindex * PGSIZE / BSIZE);
	*pte &= 0xFFF;
	*pte &= (~PTE_P);
	*pte |= PTE_AVAIL;
	*pte |= (ondiskindex<<12);
	disown(toevict);
	return toevict;
}

/*
	Called from trap.c.
	Will write back a page to memory if the page's AVAIL bit
	(which we arbitrarily designated as meaning "swapped") 
	is set.
*/
void
segflthandler(int user) {
	uint cr2 = PGROUNDDOWN(rcr2());
	pte_t* pte;
	if (user) {
		pte = walkpgdir(proc->pgdir, (void*) cr2, 0);
	}
	else if (rcr2() < KERNBASE) {
		pte = walkpgdir(proc->pgdir, (void*) cr2, 0);
	}
	if (pte && !(*pte & PTE_P) && (*pte & PTE_AVAIL)) {
		if (!unswappage(pte)) {
			proc->killed = 1;
		}
	}
	else {
		panic("In segflthandler but wrong flags in owner or no pte");
	}
}
