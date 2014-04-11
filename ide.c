// Simple PIO-based (non-DMA) IDE driver code.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "buf.h"

#define IDE_BSY       0x80
#define IDE_DRDY      0x40
#define IDE_DF        0x20
#define IDE_ERR       0x01

#define IDE_CMD_READ  0x20
#define IDE_CMD_WRITE 0x30

#define IDE_PORT_DATA     0x00
#define IDE_PORT_FEATURE  0x01
#define IDE_PORT_SECTORS  0x02
#define IDE_PORT_LBALOW   0x03
#define IDE_PORT_LBAMID   0x04
#define IDE_PORT_LBAHI    0x05
#define IDE_PORT_DRIVE    0x06
#define IDE_PORT_COMMAND  0x07

// idequeue points to the buf now being read/written to the disk.
// idequeue->qnext points to the next buf to be processed.
// You must hold idelock while manipulating queue.

static struct spinlock idelock;
static struct buf *idequeue;

static int present[4];
static void idestart(struct buf*);

static int getbaseport(int dev) {
  return (dev == 1 || dev == 0)? 0x1f0: 0x170;
}

static int getstatusport(int dev) {
  return (dev == 1 || dev == 0)? 0x3f6: 0x376;
}

void
selectdevice(int dev) {
  int baseport = getbaseport(dev);
  outb(baseport + IDE_PORT_DRIVE, 0xe0 | ((dev&1) << 4));
}

// Wait for IDE disk to become ready.
static int
idewait(int checkerr, int dev)
{
  int r;
  selectdevice(dev);
  while(((r = inb(getbaseport(dev) + IDE_PORT_COMMAND)) & (IDE_BSY|IDE_DRDY)) != IDE_DRDY) 
    ;
  if(checkerr && (r & (IDE_DF|IDE_ERR)) != 0)
    return -1;
  return 0;
}

void
ideinit(void)
{
  int dev, i, baseaddr;

  // Assume disk 0 is present
  present[0] = 1;

  initlock(&idelock, "ide");
  picenable(IRQ_IDE);
  ioapicenable(IRQ_IDE, ncpu - 1);
  idewait(0,0);
  
  for (dev = 1; dev<4; dev++) {
    selectdevice(dev);
    baseaddr = getbaseport(dev);
    for (i = 0; i<1000; i++) {
      if (inb(baseaddr + IDE_PORT_COMMAND) != 0) {
        present[dev] = 1;
        break;
      }
    }
  }
  cprintf("Disks present: 0: %d, 1: %d, 2: %d, 3: %d\n",present[0],present[1],present[2],present[3]);
  dev = 0;
  if (present[2] || (dev = present[3])) {
    picenable(IRQ_IDE2);
    ioapicenable(IRQ_IDE2, ncpu - 1);
    idewait(0,2 + dev);
  }
  
  // Switch back to disk 0.
  selectdevice(0);
}

// Start the request for b.  Caller must hold idelock.
static void
idestart(struct buf *b)
{
  int baseaddr, status;
  if(b == 0)
    panic("idestart");
  baseaddr = getbaseport(b->dev);
  status = getstatusport(b->dev);

  idewait(0, b->dev);
  outb(status, 0);  // generate interrupt 
  outb(baseaddr + IDE_PORT_SECTORS, 1);  // number of sectors
  outb(baseaddr + IDE_PORT_LBALOW, b->sector & 0xff);
  outb(baseaddr + IDE_PORT_LBAMID, (b->sector >> 8) & 0xff);
  outb(baseaddr + IDE_PORT_LBAHI, (b->sector >> 16) & 0xff);
  outb(baseaddr + IDE_PORT_DRIVE, 0xe0 | ((b->dev&1)<<4) | ((b->sector>>24)&0x0f));
  if(b->flags & B_DIRTY){
    outb(baseaddr + IDE_PORT_COMMAND, IDE_CMD_WRITE);
    outsl(baseaddr + IDE_PORT_DATA, b->data, 512/4);
  } else {
    outb(baseaddr + IDE_PORT_COMMAND, IDE_CMD_READ);
  }
}

// Interrupt handler.
void
ideintr(void)
{
  struct buf *b;

  // First queued buffer is the active request.
  acquire(&idelock);
  if((b = idequeue) == 0){
    release(&idelock);
    // cprintf("spurious IDE interrupt\n");
    return;
  }
  idequeue = b->qnext;

  // Read data if needed.
  if(!(b->flags & B_DIRTY) && idewait(1,b->dev) >= 0)
    insl(getbaseport(b->dev) + IDE_PORT_DATA, b->data, 512/4);
  
  // Wake process waiting for this buf.
  b->flags |= B_VALID;
  b->flags &= ~B_DIRTY;
  wakeup(b);
  
  // Start disk on next buf in queue.
  if(idequeue != 0)
    idestart(idequeue);

  release(&idelock);
}

//PAGEBREAK!
// Sync buf with disk. 
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
void
iderw(struct buf *b)
{
  struct buf **pp;

  if(!(b->flags & B_BUSY))
    panic("iderw: buf not busy");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(!present[b->dev])
    panic("iderw: ide disk not present");

  acquire(&idelock);  //DOC:acquire-lock

  // Append b to idequeue.
  b->qnext = 0;
  for(pp=&idequeue; *pp; pp=&(*pp)->qnext)  //DOC:insert-queue
    ;
  *pp = b;
  
  // Start disk if necessary.
  if(idequeue == b)
    idestart(b);
  
  // Wait for request to finish.
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    sleep(b, &idelock);
  }

  release(&idelock);
}

// Write whole page without interrupts.
void writepg(char* src, uint block) {
  //cprintf("Starting swap write src %p block %d\n", src, block);
  int status = getstatusport(SWAPDEV);
  int baseaddr = getbaseport(SWAPDEV);
  idewait(0,SWAPDEV);
  outb(status, 1);  // don't generate interrupt 
  outb(baseaddr + IDE_PORT_SECTORS, 8);  // number of sectors
  outb(baseaddr + IDE_PORT_LBALOW, block & 0xff);
  outb(baseaddr + IDE_PORT_LBAMID, (block >> 8) & 0xff);
  outb(baseaddr + IDE_PORT_LBAHI, (block >> 16) & 0xff);
  outb(baseaddr + IDE_PORT_DRIVE, 0xe0 | ((SWAPDEV&1)<<4) | ((block>>24)&0x0f));
  outb(baseaddr + IDE_PORT_COMMAND, IDE_CMD_WRITE);
  outsl(baseaddr + IDE_PORT_DATA, src, PGSIZE/4);
}

// Read whole page without interrupts
void readpg(char* dest, uint block) {
  //cprintf("Starting swap read dest %p block %d\n", dest, block);
  int status = getstatusport(SWAPDEV);
  int baseaddr = getbaseport(SWAPDEV);
  idewait(0,SWAPDEV);
  outb(status, 1);  // don't generate interrupt 
  outb(baseaddr + IDE_PORT_SECTORS, 8);  // number of sectors
  outb(baseaddr + IDE_PORT_LBALOW, block & 0xff);
  outb(baseaddr + IDE_PORT_LBAMID, (block >> 8) & 0xff);
  outb(baseaddr + IDE_PORT_LBAHI, (block >> 16) & 0xff);
  outb(baseaddr + IDE_PORT_DRIVE, 0xe0 | ((SWAPDEV&1)<<4) | ((block>>24)&0x0f));
  outb(baseaddr + IDE_PORT_COMMAND, IDE_CMD_READ);
  idewait(0,SWAPDEV);
  insl(getbaseport(SWAPDEV) + IDE_PORT_DATA, dest, PGSIZE/4);
}
