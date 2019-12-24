//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h" //add

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
    int fd;
    struct file *f;

    if (argint(n, &fd) < 0)
        return -1;
    if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
        return -1;
    if (pfd)
        *pfd = fd;
    if (pf)
        *pf = f;
    return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
    int fd;
    struct proc *p = myproc();

    for (fd = 0; fd < NOFILE; fd++)
    {
        if (p->ofile[fd] == 0)
        {
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

uint64
sys_dup(void)
{
    struct file *f;
    int fd;

    if (argfd(0, 0, &f) < 0)
        return -1;
    if ((fd = fdalloc(f)) < 0)
        return -1;
    filedup(f);
    return fd;
}

uint64
sys_read(void)
{
    struct file *f;
    int n;
    uint64 p;

    if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
        return -1;
    return fileread(f, p, n);
}

uint64
sys_write(void)
{
    struct file *f;
    int n;
    uint64 p;

    if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
        return -1;

    return filewrite(f, p, n);
}

uint64
sys_close(void)
{
    int fd;
    struct file *f;

    if (argfd(0, &fd, &f) < 0)
        return -1;
    myproc()->ofile[fd] = 0;
    fileclose(f);
    return 0;
}

uint64
sys_fstat(void)
{
    struct file *f;
    uint64 st; // user pointer to struct stat

    if (argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
        return -1;
    return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
    char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
    struct inode *dp, *ip;

    if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
        return -1;

    begin_op(ROOTDEV);
    if ((ip = namei(old)) == 0)
    {
        end_op(ROOTDEV);
        return -1;
    }

    ilock(ip);
    if (ip->type == T_DIR)
    {
        iunlockput(ip);
        end_op(ROOTDEV);
        return -1;
    }

    ip->nlink++;
    iupdate(ip);
    iunlock(ip);

    if ((dp = nameiparent(new, name)) == 0)
        goto bad;
    ilock(dp);
    if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0)
    {
        iunlockput(dp);
        goto bad;
    }
    iunlockput(dp);
    iput(ip);

    end_op(ROOTDEV);

    return 0;

bad:
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op(ROOTDEV);
    return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
    int off;
    struct dirent de;

    for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de))
    {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("isdirempty: readi");
        if (de.inum != 0)
            return 0;
    }
    return 1;
}

uint64
sys_unlink(void)
{
    struct inode *ip, *dp;
    struct dirent de;
    char name[DIRSIZ], path[MAXPATH];
    uint off;

    if (argstr(0, path, MAXPATH) < 0)
        return -1;

    begin_op(ROOTDEV);
    if ((dp = nameiparent(path, name)) == 0)
    {
        end_op(ROOTDEV);
        return -1;
    }

    ilock(dp);

    // Cannot unlink "." or "..".
    if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
        goto bad;

    if ((ip = dirlookup(dp, name, &off)) == 0)
        goto bad;
    ilock(ip);

    if (ip->nlink < 1)
        panic("unlink: nlink < 1");
    if (ip->type == T_DIR && !isdirempty(ip))
    {
        iunlockput(ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("unlink: writei");
    if (ip->type == T_DIR)
    {
        dp->nlink--;
        iupdate(dp);
    }
    iunlockput(dp);

    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);

    end_op(ROOTDEV);

    return 0;

bad:
    iunlockput(dp);
    end_op(ROOTDEV);
    return -1;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
    struct inode *ip, *dp;
    char name[DIRSIZ];

    if ((dp = nameiparent(path, name)) == 0)
        return 0;

    ilock(dp);

    if ((ip = dirlookup(dp, name, 0)) != 0)
    {
        iunlockput(dp);
        ilock(ip);
        if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
            return ip;
        iunlockput(ip);
        return 0;
    }

    if ((ip = ialloc(dp->dev, type)) == 0)
        panic("create: ialloc");

    ilock(ip);
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    iupdate(ip);

    if (type == T_DIR)
    {                // Create . and .. entries.
        dp->nlink++; // for ".."
        iupdate(dp);
        // No ip->nlink++ for ".": avoid cyclic ref count.
        if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
            panic("create dots");
    }

    if (dirlink(dp, name, ip->inum) < 0)
        panic("create: dirlink");

    iunlockput(dp);

    return ip;
}

uint64
sys_open(void)
{
    char path[MAXPATH];
    int fd, omode;
    struct file *f;
    struct inode *ip;
    int n;

    if ((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
        return -1;

    begin_op(ROOTDEV);

    if (omode & O_CREATE)
    {
        ip = create(path, T_FILE, 0, 0);
        if (ip == 0)
        {
            end_op(ROOTDEV);
            return -1;
        }
    }
    else
    {
        if ((ip = namei(path)) == 0)
        {
            end_op(ROOTDEV);
            return -1;
        }
        ilock(ip);
        if (ip->type == T_DIR && omode != O_RDONLY)
        {
            iunlockput(ip);
            end_op(ROOTDEV);
            return -1;
        }
    }

    if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
    {
        iunlockput(ip);
        end_op(ROOTDEV);
        return -1;
    }

    if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
    {
        if (f)
            fileclose(f);
        iunlockput(ip);
        end_op(ROOTDEV);
        return -1;
    }

    if (ip->type == T_DEVICE)
    {
        f->type = FD_DEVICE;
        f->major = ip->major;
        f->minor = ip->minor;
    }
    else
    {
        f->type = FD_INODE;
    }
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

    iunlock(ip);
    end_op(ROOTDEV);

    return fd;
}

uint64
sys_mkdir(void)
{
    char path[MAXPATH];
    struct inode *ip;

    begin_op(ROOTDEV);
    if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0)
    {
        end_op(ROOTDEV);
        return -1;
    }
    iunlockput(ip);
    end_op(ROOTDEV);
    return 0;
}

uint64
sys_mknod(void)
{
    struct inode *ip;
    char path[MAXPATH];
    int major, minor;

    begin_op(ROOTDEV);
    if ((argstr(0, path, MAXPATH)) < 0 ||
        argint(1, &major) < 0 ||
        argint(2, &minor) < 0 ||
        (ip = create(path, T_DEVICE, major, minor)) == 0)
    {
        end_op(ROOTDEV);
        return -1;
    }
    iunlockput(ip);
    end_op(ROOTDEV);
    return 0;
}

uint64
sys_chdir(void)
{
    char path[MAXPATH];
    struct inode *ip;
    struct proc *p = myproc();

    begin_op(ROOTDEV);
    if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0)
    {
        end_op(ROOTDEV);
        return -1;
    }
    ilock(ip);
    if (ip->type != T_DIR)
    {
        iunlockput(ip);
        end_op(ROOTDEV);
        return -1;
    }
    iunlock(ip);
    iput(p->cwd);
    end_op(ROOTDEV);
    p->cwd = ip;
    return 0;
}

uint64
sys_exec(void)
{
    char path[MAXPATH], *argv[MAXARG];
    int i;
    uint64 uargv, uarg;

    if (argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0)
    {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    for (i = 0;; i++)
    {
        if (i >= NELEM(argv))
        {
            goto bad;
        }
        if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0)
        {
            goto bad;
        }
        if (uarg == 0)
        {
            argv[i] = 0;
            break;
        }
        argv[i] = kalloc();
        if (argv[i] == 0)
            panic("sys_exec kalloc");
        if (fetchstr(uarg, argv[i], PGSIZE) < 0)
        {
            goto bad;
        }
    }

    int ret = exec(path, argv);

    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);

    return ret;

bad:
    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    return -1;
}

uint64
sys_pipe(void)
{
    uint64 fdarray; // user pointer to array of two integers
    struct file *rf, *wf;
    int fd0, fd1;
    struct proc *p = myproc();

    if (argaddr(0, &fdarray) < 0)
        return -1;
    if (pipealloc(&rf, &wf) < 0)
        return -1;
    fd0 = -1;
    if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0)
    {
        if (fd0 >= 0)
            p->ofile[fd0] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
        copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0)
    {
        p->ofile[fd0] = 0;
        p->ofile[fd1] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    return 0;
}

#define i2a(x) (((x) << PGSHIFT) + PHYSTOP)
#define a2i(x) ((((uint64)x) - PHYSTOP) >> PGSHIFT)

/*void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);*/
uint64
sys_mmap(void)
{

    uint64 length;
    int prot;   //记录标志位。ref. Lab9 prot indicates whether the memory should be mapped readable, writeable, and/or executable; you can assume that prot is PROT_READ or PROT_WRITE or both.
    int flags;  //记录在映射内存中的修改是否应该被写回文件。shared写回，private不写回
    int fd;     //文件描述符
    uint64 off; //文件中的偏移地址
    uint64 addr;
    struct proc *p;
    struct vm_area_struct *man; //man是所有vm_area_struct结构体的数组，linux里用链表，这里用数组。管理所有的vm_area_struct结构体
    struct file *f;             //文件指针
    int i;

    // 读取调用系统调用传入的参数，共5个
    if (argaddr(1, &length) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 ||
        argint(4, &fd) < 0 || argaddr(5, &off) < 0)
    {
        printf("sys_mmap:load argument failed\n");
        return -1;
    }

    p = myproc();    //取得当前进程
    man = &(p->man); //取得当前进程的vm_area_struct管理器。因为man是静态分配的，所以如果man不满则可以继续映射，否则，不行。

    //在当前进程的虚拟地址空间中，寻找一段空闲的满足要求的连续的虚拟地址 ref. https://blog.csdn.net/qq_33611327/article/details/81738195#mmap%E5%9F%BA%E7%A1%80%E6%A6%82%E5%BF%B5
    //如果man已满
    //man->mfiles[0].start = (void *)p->sz;  //从p->sz开始

    /*假设用户进程的地址空间无限制*/

    for (i = 0; i < NOFILE; i++)
    {
        if (man->mfiles[i].f == 0) //找到一个空闲的结构体，其文件指针file为空
            break;
    }
    if (i == NOFILE)
    {
        printf("sys_mmap:映射文件数量已满\n");
        return -1;
    }

    f = p->ofile[fd]; //取得进程打开的文件fd
    if (f == 0)
    {
        printf("sys_mmap:文件描绘符出错\n");
        return -1;
    }

    if (prot == 0) //标志位
    {
        printf("sys_mmap:标志位出错\n");
        return -1;
    }

    if (prot & PROT_READ) //只读
    {
        if (f->readable == 0)
        {
            printf("sys_mmap:file not readable\n");
            return -1;
        }
    }

    if (prot & PROT_WRITE && (flags == MAP_SHARED))
    {
        if (f->writable == 0)
        {
            printf("sys_mmap:file not writable\n");
            return -1;
        }
    }

    
    addr = p->sz;  //上一个映射文件的结束就是下一个的开始
    
    man->mfiles[i].f = f;
    man->mfiles[i].prot = prot;
    man->mfiles[i].flags = flags;
    man->mfiles[i].start = (uint64)p->sz;   //起始虚拟地址，为tail*PGSIZE+PHYSTOP，此时的尾部是上一次的尾部                           //尾部+len*PGSIZE
    man->mfiles[i].end = man->mfiles[i].start + length; //end = 起始地址+len*PGSIZE
    man->mfiles[i].off = off;
    filedup(f);                 //ref. mmap should increase the file's reference count so that the structure doesn't disappear when the file is closed (hint: see filedup).
    p->sz += length;
    return addr; //文件映射的虚拟地址
}



uint64
sys_writeback(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
    begin_op(ip->dev);                                      //ip为文件的inode，dev为设备号
    ilock(ip);                                              //写之前锁定inode
    writei(ip, 1, user_src, src, n); //writei()第2个参数，1为虚拟地址，0为物理地址
    iunlock(ip);
    end_op(ip->dev);
    return 0;
}

uint64
sys_munmap()
{
    uint64 addr;
    int length;
    struct proc *p;
    struct mfile *mfile;

    //获取参数
    argaddr(0, &addr);
    argint(1, &length);

    p = myproc();
    for (int i = 0; i < NOFILE; i++)
    {
        if (p->man.mfiles[i].f) //如果该位置的mfiles[i被使用]
        {
            mfile = &(p->man.mfiles[i]);                                                 //取得myfiles结构体
            if (addr <= mfile->start && addr + length >= mfile->end) //unmap范围是否完全覆盖
            {
                if ((mfile->flags & MAP_SHARED) && (mfile->prot & PROT_WRITE))
                {
                    //write back
                    sys_writeback(mfile->f->ip, 1, mfile->start, mfile->off, mfile->end-mfile->start); //writei()第2个参数，1为虚拟地址，0为物理地址
                }
                uvmunmap(p->pagetable, mfile->start, (mfile->end-mfile->start), 1); //vm.c中

                fileundup(mfile->f);   //undup,f->ref-1
                //这里是不是可以free一下?
                mfile->f = 0; //指针清零
                return 0;
            }
            else if (addr <= mfile->start && addr + length < mfile->end && addr + length > mfile->start) //从头开始覆盖，但末尾没有覆盖
            {
                //lower cover
                if ((mfile->flags & MAP_SHARED) && (mfile->prot & PROT_WRITE))
                {
                    //write back
                    sys_writeback(mfile->f->ip, 1, mfile->start, mfile->off, addr + length - mfile->start);
                }
                uint64 shiftup = addr + length - mfile->start; //被unmap的部分区间长度
                uvmunmap(p->pagetable, (uint64)mfile->start, shiftup, 1);
                mfile->off += shiftup;    //文件没有完全unmap，修改文件的偏移地址到已释放部分
                mfile->start += shiftup;  //文件起始位置改变
                //mfile->length -= shiftup; //文件总长度减少
                return 0;
            }
            else if (addr > mfile->start && addr + length >= mfile->end && addr < mfile->end)
            {
                //higher cover 前面每映射，后面映射的情况
                if ((mfile->flags & MAP_SHARED) && (mfile->prot & PROT_WRITE))
                {
                    //write back
                    sys_writeback(mfile->f->ip, 1, addr, mfile->off, mfile->end);
                }
                uvmunmap(p->pagetable, addr, mfile->end, 1);
                //mfile->length -= mfile->start + mfile->length - addr;  //修改文件长度
                return 0;
            }
        }
    }
    return -1;
}