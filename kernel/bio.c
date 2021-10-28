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

#define NBUCKET 13
#define HASH(id) (id%NBUCKET)

struct hashbuf{
  struct spinlock lock;
  struct buf head;
};

struct {
  struct hashbuf buckets[NBUCKET];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
} bcache;

void
binit(void)
{
  struct buf *b;
  char lockname[16];
  for(int i=0;i<NBUCKET;++i){
    snprintf(lockname,sizeof(lockname),"bcache_%d",i);
    initlock(&bcache.buckets[i].lock,lockname);
    bcache.buckets[i].head.prev=&bcache.buckets[i].head;
    bcache.buckets[i].head.next=&bcache.buckets[i].head;
  }
  for(b=bcache.buf;b<bcache.buf+NBUF;b++){
    b->next=bcache.buckets[0].head.next;
    b->prev=&bcache.buckets[0].head;
    initsleeplock(&b->lock,"buffer");
    bcache.buckets[0].head.next->prev=b;
    bcache.buckets[0].head.next=b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bid=HASH(blockno);
  acquire(&bcache.buckets[bid].lock);
  for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
  	if(b->dev==dev&&b->blockno==blockno){
		b->refcnt++;
		acquire(&tickslock);
		b->times=ticks;
		release(&tickslock);
		release(&bcache.buckets[bid].lock);
		acquiresleep(&b->lock);
		return b;
	}
  }
  struct buf *tmp;
  b=0;
  for(int i=bid,cnt=0;cnt!=NBUCKET;i=(i+1)%NBUCKET){
	++cnt;
  	if(i!=bid){
	  if(!holding(&bcache.buckets[i].lock))
	    acquire(&bcache.buckets[i].lock);
	  else
	    continue;
	}
	for(tmp=bcache.buckets[i].head.next;tmp!=&bcache.buckets[i].head;tmp=tmp->next){
	  if((tmp->refcnt==0)&&((b==0)||(tmp->times<b->times)))
		  b=tmp;
	}
	if(b){
	  if(bid!=i){
	    b->next->prev=b->prev;
	    b->prev->next=b->next;
	    release(&bcache.buckets[i].lock);
	    b->next=bcache.buckets[bid].head.next;
	    b->prev=&bcache.buckets[bid].head;
	    bcache.buckets[bid].head.next->prev=b;
	    bcache.buckets[bid].head.next=b;
	  }
	  b->dev=dev;
	  b->blockno=blockno;
	  b->valid=0;
	  b->refcnt=1;
	  acquire(&tickslock);
	  b->times=ticks;
	  release(&tickslock);
	  release(&bcache.buckets[bid].lock);
	  acquiresleep(&b->lock);
	  return b;
	}
	else{
	  if(bid!=i)
	    release(&bcache.buckets[i].lock);
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
    virtio_disk_rw(b, 0);
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
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  int bid=HASH(b->blockno);
  releasesleep(&b->lock);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  acquire(&tickslock);
  b->times=ticks;
  release(&tickslock);
  release(&bcache.buckets[bid].lock);
}

void
bpin(struct buf *b) {
  int bid=HASH(b->blockno);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt++;
  release(&bcache.buckets[bid].lock);
}

void
bunpin(struct buf *b) {
  int bid=HASH(b->blockno);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  release(&bcache.buckets[bid].lock);
}


