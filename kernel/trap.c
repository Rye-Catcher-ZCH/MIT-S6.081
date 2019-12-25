#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"


// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..39 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..12 -- 12 bits of byte offset within the page.

/*
static pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}
*/

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}


//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void usertrap(void)
{
    int which_dev = 0;

    if ((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    w_stvec((uint64)kernelvec);

    struct proc *p = myproc(); //myproc()函数获取当前进程的结构体，定义在proc.c中

    // save user program counter.
    p->tf->epc = r_sepc();

    // r_scause返回进入user_trap()的原因
    if (r_scause() == 8)
    {
        // system call

        if (p->killed) //杀死进程
            exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->tf->epc += 4;

        // an interrupt will change sstatus &c registers,
        // so don't enable until done with those registers.
        intr_on();

        syscall();
    }
    //ref. hint1:a fault is a page fault by seeing if r_scause() is 13 or 15 in usertrap().
    else if (r_scause() == 13 || r_scause() == 15)
    {
        //r_scause() == 13为页面加载错误(load)
        //r_scause() == 15为页面写回错误(store)
        //一个严重的问题，这里不需要再调用myproc()了!!

        //proc为进程控制块PCB，其结构定义在proc.h中，现在取得该进程对应的页表
        pagetable_t cur_pagetable = p->pagetable;
        //找到发生缺页错误的虚拟地址，调用r_stval()函数，定义在riscv.h中
        //stval=0x0000000000004008，虚拟地址为4*16 = 64位
        uint64 fault_vaddr = (uint64)r_stval();
        //ref. hint4:使用PGROUNDDOWN(vaddr)将有问题的虚拟地址向下舍入到页面边界(也是一个虚拟地址)。
        //PGROUNDDOWN()定义在riscv.h中
        uint64 fault_page = PGROUNDDOWN(fault_vaddr);
        //定义指针指向分配的内存空间
        uint64 *mem;

        //下面开始做错误处理
        //对于每个进程至少分配两个页，其中一个页用作保护，具体结构如下
        /* 保护页(page1)
           栈顶(page2)
           栈低(page)
        */
        //如果错误虚拟地址进入保护区(小于栈顶地址)，并且大于栈顶地址-PGSIZE
        //需要修改proc结构，增加stack_top，记录进程用户栈栈顶地址
        if (fault_vaddr < p->stack_top && fault_vaddr >= p->stack_top - PGSIZE)
        {
            //非法内存访问，杀死进程，退出
            p->killed = 1;
            //printf("the top = %d\n", p->stack_top);
            //printf("the bottom = %d\n", p->kstack);
            printf("usertrap(): 非法访问保护区，访问地址小于栈顶\n");
            exit(-1);
        }
        //如果错误虚拟地址大于进程最大
        //注意到需要特判 pid = 1 的情况，是因为 xv6 的 initcode 中引用了大于用户线性空间最大值,触发一个误判.
        if (fault_vaddr >= p->sz)
        {
            p->killed = 1;
            printf("usertrap(): 访问超出了线性地址的有效范围\n");
            exit(-1);
        }

        //调用kalloc()分配内存
        //ref. hint3:
        //Steal code from uvmalloc() in vm.c, which is what sbrk() calls (via growproc()). You'll need to call kalloc() and mappages().
        //walk定义在上面，在页表p->pagetable中返回与虚拟地址fault_vaddr对应的PTE地址。alloc=1，创建任何必需的页表页。
        //对这里进行了修改，取消了强转uint64*
        //pte_t *pte = walk(p->pagetable, fault_vaddr, 1); //递归查找页表项
        //if (!(*pte & PTE_V))                             //如果页表页无效，没有被映射，防止remap
        //{
            mem = kalloc(); //分配一个页的内存空间
            //如果内存分配失败，退出
            if (!mem)
            {
                printf("usertrap():内存空间分配失败\n");
                p->killed = 1;
                exit(-1);
            }

            //初始化一个页面大小的内存，PGSIZE=4kb，定义在riscv.h中
            memset(mem, 0, PGSIZE);

            //为以fault_page开头的虚拟地址创建PTE，定义在vm.c中
            //如果创建页表项失败，则将申请的地址空间释放
            if (mappages(cur_pagetable, fault_page, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0)
            {
                printf("usertrap():无法将一个物理页映射到页表中\n");
                kfree(mem);
                p->killed = 1;
                exit(-1);
            }
        //}
    }
    else if ((which_dev = devintr()) != 0)
    {
        // ok
        if (p->killed)
            exit(-1);
    }
    else
    {
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
        p->killed = 1;
        exit(-1); //这里要加上exit(-1)否则出错
    }

    /*
    if (p->killed)
        exit(-1);
    */

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2)
        yield();

    usertrapret();
}



//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // turn off interrupts, since we're switching
  // now from kerneltrap() to usertrap().
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->tf->kernel_satp = r_satp();         // kernel page table
  p->tf->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->tf->kernel_trap = (uint64)usertrap;
  p->tf->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->tf->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
// must be 4-byte aligned to fit in stvec.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ || irq == VIRTIO1_IRQ ){
      virtio_disk_intr(irq - VIRTIO0_IRQ);
    }

    plic_complete(irq);
    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

