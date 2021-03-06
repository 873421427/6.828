// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock locks[3];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  struct buf heads[3];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.locks[0], "bcache0");
  initlock(&bcache.locks[1], "bcache1");
  initlock(&bcache.locks[2], "bcache2");

  // Create linked list of buffers
  bcache.heads[0].prev = &bcache.heads[0];
  bcache.heads[0].next = &bcache.heads[0];
  bcache.heads[1].prev = &bcache.heads[1];
  bcache.heads[1].next = &bcache.heads[1];
  bcache.heads[2].prev = &bcache.heads[2];
  bcache.heads[2].next = &bcache.heads[2];

  int i = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.heads[i].next;
    b->prev = &bcache.heads[i];
    initsleeplock(&b->lock, "buffer");
    bcache.heads[i].next->prev = b;
    bcache.heads[i].next = b;
    i = (i+1) % 3;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  int i = blockno % 3;
  struct buf *b;

  acquire(&bcache.locks[i]);

  // Is the block already cached?
  for(b = bcache.heads[i].next; b != &bcache.heads[i]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.locks[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached; recycle an unused buffer.
  for(b = bcache.heads[i].prev; b != &bcache.heads[i]; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.locks[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b->dev, b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b->dev, b, 1);
}

// Release a locked buffer.
// Move to the head of the MRU list.
void
brelse(struct buf *b)
{
  int i = b->blockno % 3;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.locks[i]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.heads[i].next;
    b->prev = &bcache.heads[i];
    bcache.heads[i].next->prev = b;
    bcache.heads[i].next = b;
  }
  
  release(&bcache.locks[i]);
}

void
bpin(struct buf *b) {
  int i = b->blockno % 3;
  acquire(&bcache.locks[i]);
  b->refcnt++;
  release(&bcache.locks[i]);
}

void
bunpin(struct buf *b) {
  int i = b->blockno % 3;
  acquire(&bcache.locks[i]);
  b->refcnt--;
  release(&bcache.locks[i]);
}


