#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int pd[2])
{
  int pd_2[2];  //创建一个新管道

  int p; //是一个质数
  int n; //
  int f; //存储fork()的返回值
  close(pd[1]);  //关闭子进程中pd（左）管道的写描述符(此时关闭了三个，还剩子进程中的管道读描述符)。
  read(pd[0], &p, sizeof(p));  //读入输入管道的第一个字符，必然是质数
  printf("prime %d\n", p);

  if (p == 31)  //临时条件，==31则退出，类似于递归终止条件
  {
    close(pd[0]);
    close(pd[1]);
    exit();
  }
  pipe(pd_2);  //构建管道2
  if ((f = fork()) > 0)  //fork子进程
  {
    while (read(pd[0], &n, sizeof(n)))  //如果左侧管道没有读取完成，则读取
    {
      //printf("\n");
      if (n % p != 0)  //不是p的倍数  
        write(pd_2[1], &n, sizeof(n));  //传到右侧管道
    }
    close(pd_2[0]);  //关闭父进程中pd_2的读描述符（关闭1/4）
    close(pd[0]);  //关闭子进程中pd（左）管道的读描述符，此时对于pd管道，其四个描述符都被关闭。
    close(pd_2[1]);  //关闭父进程中pd_2的写描述符（关闭2/4）
    wait();  //等待创建的子进程结束
    exit();  //退出
  }
  else if (f == 0)  //是子进程
  {
    sieve(pd_2);  //递归处理
    exit();  //退出
  }
  else  //fork失败，输出error，该环境prime=17时，似乎不能再fork
  {
    printf("error\n");
  }
}

int main()
{
  int pd[2];
  int i = 0;
  pipe(pd);  //创建一个管道

  if (fork())  //fork子进程
  {
    for (i = 2; i < 36; i++)
    {
      write(pd[1], &i, sizeof(i));  //父进程逐个将数字写入管道pd
    }
    //如果关掉再fork()那么子进程中端口也是关闭的，关闭后一般无法再打开
    close(pd[0]);  //关闭读端口，对于父子进程，如果关掉父进程的读端口，不影响子进程读端口！
    close(pd[1]);  //之前一直错误理解了，关闭读端口，读到的都是0，关闭写端口，则最后会添加一个EOF
    wait();  //等待创建的子进程结束
    exit();  //另外的环境exit和wait都不需要参数
  }
  else
  {
    sieve(pd);  //子进程开始逐个读出数字，并处理
    exit();
  }
}
