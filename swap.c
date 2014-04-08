#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
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

void
scnodeenqueue(void* va) {
	uint idx = v2p(va)/PGSIZE;
	struct scnode* slot = &(nodememory[idx]);
	slot->index = idx;
	(schead.next)->prev = slot;
	slot->next = schead.next;
	slot->prev = &schead;
	schead.next = slot;
}

void
scnodedequeue(void* va) {
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