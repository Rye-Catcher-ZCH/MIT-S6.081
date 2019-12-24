//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];

//list定义在defs.h中，如下所示，两个指针
/*
struct list {
  struct list *next;
  struct list *prev;
};
*/

struct file_list_node
{
  struct file file;      //文件结构体
  struct list file_list; //指向下一个和上一个file_list_node的指针，类似于侵入式链表
};

struct
{
  struct spinlock lock;
  struct list file; //创建文件链表，替换file数组
  // struct file file[NFILE];
} ftable;

void fileinit(void)
{
  initlock(&ftable.lock, "ftable");
  lst_init(&(ftable.file)); //初始化文件链表
}

// Allocate a file structure.
struct file *filealloc(void)
{
  struct file_list_node *f;                     //创建一个新的文件结点
  f = bd_malloc(sizeof(struct file_list_node)); //为新的结点分配空间
  if (f)                                        //结点空间分配成功
  {
    if (f->file.ref == 0)
      f->file.ref = 1; //修改文件的引用次数为1

    acquire(&ftable.lock);                     //锁定ftable
    lst_push(&(ftable.file), &(f->file_list)); //将f->file_list链接到ftable.file链表中。
    release(&ftable.lock);
    //这里将file_struct_node*类型强转成struct file*类型，按函数要求返回
    //反强转就可取得file_struct_node??
    return ((struct file*)f);  //为什么返回(struct file *)fl;？不应该返回整个链表结构的首地址？按函数要求返回分配的文件指针
    //报错：returning 'struct list *' from a function with incompatible return type 'struct file *'
  }
  else
  {
    printf("allocate error!\n");
    return 0;
  }

  /*
  acquire(&ftable.lock);
  for (f = ftable.file; f < ftable.file + NFILE; f++)
  {
    if (f->ref == 0)
    {
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  */
  // return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

/// Close file f.  (Decrement ref count, close when reaches 0.)
void fileclose(struct file *f)
{
  struct file ff;
  struct file_list_node *f_list_node;
  acquire(&ftable.lock);
  if (f->ref < 1)
    panic("fileclose");
  if (--f->ref > 0) //如果文件的引用次数-1后，仍然>0
  {
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;                               //对于数组，f->ref改为0，就释放，但对于链表需要remove和free
  f->type = FD_NONE;                        //FD_NONE定义在file.h中，作用是什么呢？
  f_list_node = (struct file_list_node *)f; //将文件指针f强转成链表结点file_list_node？可以这样吗
  lst_remove(&(f_list_node->file_list));    //将该文件对应的侵入式链表移除
  bd_free(f_list_node);                     //释放该文件结点的内存
  release(&ftable.lock);

  if (ff.type == FD_PIPE)
  {
    pipeclose(ff.pipe, ff.writable);
  }
  else if (ff.type == FD_INODE || ff.type == FD_DEVICE)
  {
    begin_op(ff.ip->dev);
    iput(ff.ip);
    end_op(ff.ip->dev);
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op(f->ip->dev);
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op(f->ip->dev);

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

