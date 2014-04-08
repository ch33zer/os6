struct scnode {
	struct scnode* next;
	struct scnode* prev;
	uint index; //Physical address (like v2p(kalloc())) divided by PGSIZE
};

struct freeswapnode {
	struct freeswapnode* next;
	uint index;
};

#define SWAPPGCAPACITY (TOTALSWAPBYTES/PGSIZE)

#define MEMORYPGCAPACITY (TOTALMAINBYTES/PGSIZE)

#define PG_UNOWNED 0