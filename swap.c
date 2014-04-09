#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "fs.h"
#include "x86.h"
#include "proc.h"
#include "swap.h"

static struct scnode schead;
static struct scnode sctail;

static struct scnode nodememory[MEMORYPGCAPACITY];

static struct freeswapnode* swaphead;

static struct freeswapnode swapmemory[SWAPPGCAPACITY];

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
	curr->index = i;

	// Setup second chance queue
	schead.next = &sctail;
	sctail.prev = &schead;
}

int isreferenced(uint va) {
	return *(owner[va]) & PTE_A;
}

void setunreferenced(uint va) {
	*(owner[va]) &= ~(PTE_A);
}

char*
choosepageforeviction(void) {
	struct scnode* curr = schead.next;
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

	return p2v(curr->index * PGSIZE); // This is the virtual address of the page that will be evicted
}

void
scnodeenqueue(void* va) {
	uint idx = v2p(va)/PGSIZE;
	struct scnode* slot = &(nodememory[idx]);
	slot->index = idx;
	(sctail.prev)->next = slot;
	slot->next = &sctail;
	slot->prev = sctail.prev;
	sctail.prev = slot;
}

void
scnoderemove(void* va) {
	uint idx = v2p(va)/PGSIZE;
	struct scnode* slot = &(nodememory[idx]);
	struct scnode* prev = slot->prev;
	struct scnode* next = slot->next;
	prev->next = next;
	next->prev = prev;
}

struct freeswapnode*
freeswapalloc(void) {
	struct freeswapnode* node;
	if (swaphead) {
		node = swaphead;
		swaphead = swaphead->next;
		return node;
	}
	else {
		return 0;
	}
}

void
freeswapfree(uint index) {
	struct freeswapnode* node;
	node = &(swapmemory[index]);
	node->next = swaphead;
	swaphead = node;
}

struct freeswapnode*
getfreenode() {
	if (!swaphead) {
		panic("Out of memory and swap pages");
	}
	struct freeswapnode* node = swaphead;
	swaphead = swaphead->next;
	return node;
}

uint
evict(char* pgsrc) {
	struct freeswapnode* node = getfreenode();
	uint idx = node->index;
	writepg(pgsrc, idx * PGSIZE / BSIZE);
	return idx;
}

void
segflthandler() {
	uint cr2 = PGROUNDDOWN(rcr2());
	pte_t* pte = walkpgdir(proc->pgdir, (void*) cr2, 0);
	if (pte) {
		char* newmem = kalloc(1);
		uint diskidx = ((uint)*pte) >> 12;
		uint flags = ((uint)*pte) & 0xFFF;
		flags |= PTE_P;
		readpg(newmem, diskidx * PGSIZE / BSIZE);
		*pte = 0;
		mappages(proc->pgdir, (void*)cr2, PGSIZE, v2p(newmem), flags);
	}
}