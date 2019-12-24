#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// Buddy allocator

static int nsizes; // the number of entries in bd_sizes array

#define LEAF_SIZE 16                          // The smallest block size
#define MAXSIZE (nsizes - 1)                  // Largest index in bd_sizes array
#define BLK_SIZE(k) ((1L << (k)) * LEAF_SIZE) // Size of block at size k 1L为long型的变量，计算block的大小
#define HEAP_SIZE BLK_SIZE(MAXSIZE)
#define NBLK(k) (1 << (MAXSIZE - k))                   // Number of block at size k
#define ROUNDUP(n, sz) (((((n)-1) / (sz)) + 1) * (sz)) // Round up to the next multiple of sz

typedef struct list Bd_list;

// The allocator has sz_info for each size k. Each sz_info has a free
// list, an array alloc to keep track which blocks have been
// allocated, and an split array to to keep track which blocks have
// been split.  The arrays are of type char (which is 1 byte), but the
// allocator uses 1 bit per block (thus, one char records the info of
// 8 blocks).
/*
对于每个大小k，分配器都有sz_info。
每个sz_info都有一个空闲列表，一个数组alloc来跟踪已分配的块，以及一个split数组来跟踪已拆分的块。 
数组的类型为char（1个字节），但是分配器每块使用1位（因此，一个char记录8个块的信息）。
*/
struct sz_info
{
    Bd_list free;
    char *alloc;
    char *split;
};
typedef struct sz_info Sz_info;

static Sz_info *bd_sizes;
static void *bd_base; // start address of memory managed by the buddy allocator，伙伴分配器管理内存的起始地址
static struct spinlock lock;

// Return 1 if bit at position index in array is set to 1
// 检查是否被分配函数，某一位置内存被分配则返回1
int bit_isset(char *array, int index)
{
    // array是char型数组，而每个char变量占据8位，每一个位可以管理一个块。
    // 所以逻辑是先通过index/8找到那个char变量，再通过index%8找到char变量的某一位。
    // eg：index = 28，28/8=3找到第3个char型变量，28%8=4找到第3个char型变量的第4位
    // 1<<4 = 00010000,然后和char型变量做与运算，找到第28个块是否被分配。
    // 未被分配00000000 != 00010000
    char b = array[index / 8];
    char m = (1 << (index % 8));
    return (b & m) == m;
}

// Set bit at position index in array to 1
// 分配内存函数，将某一位置的索引设置为1
void bit_set(char *array, int index)
{
    char b = array[index / 8];
    char m = (1 << (index % 8));
    array[index / 8] = (b | m); //set某一位为1，直接原char变量和m做或运算即将那一位(对应一个块)设为1
}

/*新函数，解决位的xor(异或)运算*/
void bit_xor(char *array, int index)
{
    char b = array[index / 8];
    char m = (1 << (index % 8)); // 或运算变为xor运算
    array[index / 8] = (b ^ m);
}

// Clear bit at position index in array
void bit_clear(char *array, int index)
{
    char b = array[index / 8];
    char m = (1 << (index % 8));
    array[index / 8] = (b & ~m); //清空char变量中的某一位
}

// Print a bit vector as a list of ranges of 1 bits
// 一个打印函数，用于调试
void bd_print_vector(char *vector, int len)
{
    int last, lb;

    last = 1;
    lb = 0;
    for (int b = 0; b < len; b++)
    {
        if (last == bit_isset(vector, b))
            continue;
        if (last == 1)
            printf(" [%d, %d)", lb, b);
        lb = b;
        last = bit_isset(vector, b);
    }
    if (lb == 0 || last == 1)
    {
        printf(" [%d, %d)", lb, len);
    }
    printf("\n");
}

// Print buddy's data structures
// 打印伙伴分配器的数据结构
void bd_print()
{
    for (int k = 0; k < nsizes; k++)
    {
        printf("size %d (blksz %d nblk %d): free list: ", k, BLK_SIZE(k), NBLK(k));
        lst_print(&bd_sizes[k].free);
        printf("  alloc:");
        bd_print_vector(bd_sizes[k].alloc, NBLK(k));
        if (k > 0)
        {
            printf("  split:");
            bd_print_vector(bd_sizes[k].split, NBLK(k));
        }
    }
}

// What is the first k such that 2^k >= n?
// 伙伴分配器按照2的幂划分内存，eg:需求66kb，则需要划分128kb，128>66,64<66
// 所以内部碎片会较多
// 该函数的功能就是找到>66kb的最近的一个k值
// 参考:https://blog.csdn.net/wojiuguowei/article/details/79377228
int firstk(uint64 n)
{
    int k = 0;
    uint64 size = LEAF_SIZE;

    while (size < n)
    {
        k++;
        size *= 2;
    }
    return k;
}

// Compute the block index for address p at size k
// 给定一个地址p，计算从内存起始地址到地址p包含几个大小为k(注意这里k需要经过宏定义计算，是真实大小)的块
int blk_index(int k, char *p)
{
    int n = p - (char *)bd_base;
    return n / BLK_SIZE(k); //BLK_SIZE为宏定义，#define BLK_SIZE(k) ((1L << (k)) * LEAF_SIZE)
}

// Convert a block index at size k back into an address
// 给定一个块的块号，和块的size(k)，找到块的物理地址(从内存起始地址开始)
void *addr(int k, int bi)
{
    int n = bi * BLK_SIZE(k);
    return (char *)bd_base + n;
}

// allocate nbytes, but malloc won't return anything smaller than LEAF_SIZE
// 分配一段内存，分配内存大小至少为LEAF_SIZE
void *
bd_malloc(uint64 nbytes)
{
    int fk, k;

    acquire(&lock);

    // Find a free block >= nbytes, starting with smallest k possible
    /*假如系统需要4(2*2)个页面大小的内存块，该算法就到free_area[2]中查找，如果链表中有空闲块，就直接从中摘下并分配出去。
    如果没有，算法将顺着数组向上查找free_area[3],如果free_area[3]中有空闲块，则将其从链表中摘下，分成等大小的两部分，
    前四个页面作为一个块插入free_area[2]，后4个页面分配出去。
    free_area[3]中也没有，就再向上查找，如果free_area[4]中有，就将这16(2*2*2*2)个页面等分成两份，前一半挂如free_area[3]的链表头部，
    后一半的8个页等分成两等分，前一半挂free_area[2]的链表中，后一半分配出去。
    假如free_area[4]也没有，则重复上面的过程，直到到达free_area数组的最后，如果还没有则放弃分配。
    https://blog.csdn.net/orange_os/article/details/7392986
  */
    fk = firstk(nbytes); //最小满足(128之于66)，这里小的不行，分配大的？
    for (k = fk; k < nsizes; k++)
    {
        //如果size为k的块没有空闲，则break。
        //对于一个完全没有分配的内存，最终肯定要找到最大的那一块内存，然后将最大的内存eg:1024分裂为两个512，一个挂载到sz=512的链表，另一个继续分配
        if (!lst_empty(&bd_sizes[k].free)) // bd_size定义在全局，对每个大小k，都有相应的管理结构体，是SZ_info类型
            break;
    }
    if (k >= nsizes)
    { // No free blocks?
        release(&lock);
        return 0;
    }

    // Found a block; pop it and potentially split it
    char *p = lst_pop(&bd_sizes[k].free); //找到一个空闲块
    /*self_change*/
    // bit_set(bd_sizes[k].alloc, blk_index(k, p));
    // 首先找到记录大小为k的块分配信息的数组bd_sizes[k].alloc，然后通过blk_index找到是第几个大小为k的块
    // /2源于伙伴分配，0和1是伙伴、2和3是伙伴、4和5是伙伴…… 0/2=0且1/2=0,2/2=1且3/2=1，所以按照题目要求，两个块占据一位，做xor操作
    bit_xor(bd_sizes[k].alloc, blk_index(k, p) / 2);
    /*self_change*/

    // 在k到最小分配前(128之于66)，将其一分为二，一部分挂载到空闲链表，并在split中记录，另一部分继续split并分配
    for (; k > fk; k--)
    {
        // split a block at size k and mark one half allocated at size k-1
        // and put the buddy on the free list at size k-1
        char *q = p + BLK_SIZE(k - 1);               // p's buddy  q指向p一分为二的后一个块，该块被挂载到sz=k-1的空闲链表上
        bit_set(bd_sizes[k].split, blk_index(k, p)); //设置大小为k的这一个块被split
        /*self_change*/
        // bit_set(bd_sizes[k-1].alloc, blk_index(k-1, p));  //原始表示sz为k-1大小的这一块被分配，因为请求的内存会占据一部分，不能分配整块2*^k-1的内存空间
        bit_xor(bd_sizes[k - 1].alloc, blk_index(k - 1, p) / 2); //现在不直接set=1，而是和他的伙伴块做xor。
        /*self_change*/
        lst_push(&bd_sizes[k - 1].free, q); //p的后一半被挂载到sz=k-1的空闲链表上
    }
    release(&lock);

    return p;
}

// Find the size of the block that p points to.
// 找到内存地址为p所指向的块的大小。
int size(char *p)
{
    for (int k = 0; k < nsizes; k++)
    {
        if (bit_isset(bd_sizes[k + 1].split, blk_index(k + 1, p)))
        {
            return k;
        }
    }
    return 0;
}

// Free memory pointed to by p, which was earlier allocated using
// bd_malloc.
// 释放p指向的内存空间，该空间被bd_malloc分配。注意释放空间时要尽可能合并空间。eg:两个伙伴32合并为一个64
void bd_free(void *p)
{
    void *q;
    int k;

    acquire(&lock);
    for (k = size(p); k < MAXSIZE; k++) //k为p指向的块的大小
    {
        int bi = blk_index(k, p);                    //找到这个块的index
        int buddy = (bi % 2 == 0) ? bi + 1 : bi - 1; //找到该块的buddy块号，如果是偶数则+1，如果是奇数则-1
        /*self_change*/
        // bit_clear(bd_sizes[k].alloc, bi);  // free p at size k
        // 注意到我们只是为了节省alloc数组的空间(只用原来的一般)
        // 但是对于bi来说，它的buddy块的index为bi+1或bi-1
        // 即4、5互为buddy块，但是它们仅占用alloc数组中的第4/2=2位
        bit_xor(bd_sizes[k].alloc, bi / 2); //和buddy做xor运算
        if (bit_isset(bd_sizes[k].alloc, bi / 2))
        {
            // 如果buddy被分配则break，不用合并
            break; // break out of loop
        }
        // budy is free; merge with buddy，否则buddy为空，合并
        q = addr(k, buddy); //找到buddy的地址
        lst_remove(q);      // remove buddy from free list将buddy从sz = k的链表中移除
        if (buddy % 2 == 0) //buddy为偶数，则合并后的起始地址为buddy的地址q
        {
            p = q;
        }
        // at size k+1, mark that the merged buddy pair isn't split
        // anymore
        // 和buddy合并，清除split标记位
        bit_clear(bd_sizes[k + 1].split, blk_index(k + 1, p));
    }
    lst_push(&bd_sizes[k].free, p); //将合并后的块挂载到sz = k的空闲链表是(注意for循环退出时，k变大)
    release(&lock);
}

// Compute the first block at size k that doesn't contain p
// 计算地址p的下一个大小为k的块索引
int blk_index_next(int k, char *p)
{
    int n = (p - (char *)bd_base) / BLK_SIZE(k);
    if ((p - (char *)bd_base) % BLK_SIZE(k) != 0)
        n++;
    return n;
}

// 计算log2的近似值，保留整数
int log2(uint64 n)
{
    int k = 0;
    while (n > 1)
    {
        k++;
        n = n >> 1;
    }
    return k;
}

// Mark memory from [start, stop), starting at size 0, as allocated.
// 标记start到stop的块为被分配
void bd_mark(void *start, void *stop)
{
    int bi, bj;

    if (((uint64)start % LEAF_SIZE != 0) || ((uint64)stop % LEAF_SIZE != 0))
        panic("bd_mark");

    for (int k = 0; k < nsizes; k++)
    {
        bi = blk_index(k, start);
        bj = blk_index_next(k, stop);
        for (; bi < bj; bi++)
        {
            if (k > 0)
            {
                // if a block is allocated at size k, mark it as split too.
                // 一个在size k被分配的块，也标记为被split
                bit_set(bd_sizes[k].split, bi);
            }
            /*self_change*/
            // bit_set(bd_sizes[k].alloc, bi);
            bit_xor(bd_sizes[k].alloc, bi / 2); //和伙伴做xor(共用一个位置)
                                                /*self_change*/
        }
    }
}

// If a block is marked as allocated and the buddy is free, put the
// buddy on the free list at size k.
// 如果一个块被分配，它的buddy空闲，则将buddy挂载到对应大小的空闲链表上
int bd_initfree_pair(int k, int bi, void *allow_left, void *allow_right)
{
    int buddy = (bi % 2 == 0) ? bi + 1 : bi - 1;
    int free = 0;
    /*self_change*/
    // if(bit_isset(bd_sizes[k].alloc, bi) !=  bit_isset(bd_sizes[k].alloc, buddy)) {
    if (bit_isset(bd_sizes[k].alloc, bi / 2)) //如果alloc为1(异或运算为1说明两个块状态不一致，有一个为空)
    {
        // one of the pair is free
        free = BLK_SIZE(k);
        // if(bit_isset(bd_sizes[k].alloc, bi))
        // 如果buddy在合法地址内，那么将buddy挂载到空闲链表上，否则bi必在合法地址内(因为两个中有一个空闲)，则将其挂载到空闲链表
        if (addr(k, buddy) <= allow_right && addr(k, buddy) >= allow_left)
            lst_push(&bd_sizes[k].free, addr(k, buddy)); // put buddy on free list
        else
            lst_push(&bd_sizes[k].free, addr(k, bi)); // put bi on free list
    }
    return free;
}

// 问问题
// Initialize the free lists for each size k.  For each size k, there
// are only two pairs that may have a buddy that should be on free list:
// bd_left and bd_right.
// bd_left为管理内存空间的最小地址、bd_right为最大地址
int bd_initfree(void *bd_left, void *bd_right)
{
    int free = 0;
    //  MAXSIZE为Sz_info数组的最大索引
    for (int k = 0; k < MAXSIZE; k++)
    { // skip max size
        // 找到伙伴分配器管理内存空间的最小块索引和最大块索引，因为有一些内存空间标记为已分配。
        // 对于标记为已分配内存空间中内存块，他们的buddy可能处于合法范围并且是free的
        // bd_initfree_pair()函数将这些buddy添加到空闲链表中。
        int left = blk_index_next(k, bd_left);                //起始块的索引
        int right = blk_index(k, bd_right);                   //终止块的索引
        free += bd_initfree_pair(k, left, bd_left, bd_right); //处理 [base, p)的allocated块
        if (right <= left)
            continue;                                          //若right<left则没有必要在处理右侧的块
        free += bd_initfree_pair(k, right, bd_left, bd_right); //处理[end, HEAP_SIZE)的allcated块
    }
    return free;
}

// Mark the range [bd_base,p) as allocated
int bd_mark_data_structures(char *p)
{
    int meta = p - (char *)bd_base;
    printf("bd: %d meta bytes for managing %d bytes of memory\n", meta, BLK_SIZE(MAXSIZE));
    bd_mark(bd_base, p);
    return meta;
}

// Mark the range [end, HEAPSIZE) as allocated
int bd_mark_unavailable(void *end, void *left)
{
    int unavailable = BLK_SIZE(MAXSIZE) - (end - bd_base);
    if (unavailable > 0)
        unavailable = ROUNDUP(unavailable, LEAF_SIZE);
    printf("bd: 0x%x bytes unavailable\n", unavailable);

    void *bd_end = bd_base + BLK_SIZE(MAXSIZE) - unavailable;
    bd_mark(bd_end, bd_base + BLK_SIZE(MAXSIZE));  //标记为已分配，都已表示为LEAF_SIZE的倍数
    return unavailable;  //表示为LEAF_SIZE的倍数
}

// Initialize the buddy allocator: it manages memory from [base, end).
// 初始化伙伴分配器，管理的地址空间是base到end
// base和end还没有变为LEAF_SIZE的倍数
void bd_init(void *base, void *end)
{
    char *p = (char *)ROUNDUP((uint64)base, LEAF_SIZE); //将base变为LEAF_SIZE的倍数
    int sz;

    initlock(&lock, "buddy");
    bd_base = (void *)p;

    // compute the number of sizes we need to manage [base, end)
    // 通过log2计算最大的nsizes(计算二叉树层数)
    nsizes = log2(((char *)end - p) / LEAF_SIZE) + 1;
    if ((char *)end - p > BLK_SIZE(MAXSIZE))
    {
        nsizes++; // round up to the next power of 2 向上取整
    }

    printf("bd: memory sz is %d bytes; allocate an size array of length %d\n",
           (char *)end - p, nsizes);

    // allocate bd_sizes array
    bd_sizes = (Sz_info *)p;                       //Sz_info数组
    p += sizeof(Sz_info) * nsizes;                 //p指向数组的尾部
    memset(bd_sizes, 0, sizeof(Sz_info) * nsizes); //初始化空闲链表空间为0

    // initialize free list and allocate the alloc array for each size k
    // 初始化空闲链表和每个size的分配数组
    for (int k = 0; k < nsizes; k++)
    {
        lst_init(&bd_sizes[k].free);
        sz = sizeof(char) * ROUNDUP(NBLK(k), 8) / 8; //两个宏定义NBLK计算块数、round up四舍五入到8的下一个倍数
        /*self_change*/
        sz = (sz - 1) / 2 + 1; //采用伙伴分配器，两个块占用一个为，可以减小内存分配为原来的1/2
        /*self_change*/
        bd_sizes[k].alloc = p;            //分配alloc数组的起始地址
        memset(bd_sizes[k].alloc, 0, sz); //alloc数组初始化为0
        p += sz;                          //p指向下一个空闲区域
    }

    // allocate the split array for each size k, except for k = 0, since
    // we will not split blocks of size k = 0, the smallest size.
    // 初始化split数组，size=0，没有split数组
    for (int k = 1; k < nsizes; k++)
    {
        sz = sizeof(char) * (ROUNDUP(NBLK(k), 8)) / 8;
        bd_sizes[k].split = p;
        memset(bd_sizes[k].split, 0, sz);
        p += sz;
    }
    p = (char *)ROUNDUP((uint64)p, LEAF_SIZE);

    // done allocating; mark the memory range [base, p) as allocated, so
    // that buddy will not hand out that memory.
    // [base,p)为伙伴分配器的数据结构，标记为已分配，伙伴分配器不管理这部分内存
    int meta = bd_mark_data_structures(p);

    // mark the unavailable memory range [end, HEAP_SIZE) as allocated,
    // so that buddy will not hand out that memory.
    // [end,HEAP_SIZE)为不可达内存，标记为已分配，伙伴分配器不管理这部分内存
    int unavailable = bd_mark_unavailable(end, p);
    void *bd_end = bd_base + BLK_SIZE(MAXSIZE) - unavailable;

    // initialize free lists for each size k
    // 初始化每个size的空闲链表，管理p到bd_end的内存空间
    int free = bd_initfree(p, bd_end);

    // check if the amount that is free is what we expect
    // 检查空闲空间是否与我们期望的一致
    if (free != BLK_SIZE(MAXSIZE) - meta - unavailable)
    {
        printf("free %d %d\n", free, BLK_SIZE(MAXSIZE) - meta - unavailable);
        panic("bd_init: free mem");
    }
}
