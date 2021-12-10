#ifndef BUF_H
#define BUF_H

#include "types.h"
#include "sleeplock.h"
#include "param.h"
#include "fs.h"

struct buf {
  int flags;
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *qnext; // disk queue
  uchar data[BSIZE];
};

#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

void buffers_init();
void buffer_release(struct buf*);
struct buf* buffer_read(uint, uint);
void buffer_write(struct buf*);

#endif