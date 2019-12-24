#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    char buf2[512];
    char buf[32][32];
    char *pass[32];
    int i;
    for (i = 0; i < 32; i++) //初始化指针数组pass，使第i个pass指针 指向buf的第i行。
        pass[i] = buf[i];

    for (i = 1; i < argc; i++) //读入命令行传入的参数argv，参数的个数为argc，忽略第一个参数，第一个参数为运行文件名xargs
    {
        strcpy(buf[i - 1], argv[i]); //将读入的参数存到buf里, eg：传入xargs echo bye 则buf中存储为echo 和 bye
    }

    int n;
    /*程序运行到这里后，等待用户输入*/
    while ((n = read(0, buf2, sizeof(buf2))) > 0) //读入用户键盘输入（大小为512字节），存入buf2中
    {
        int pos = argc - 1; //xargs echo bye argc=3，但xargs不存储,所以buf中只有两个参数,argc-1指向一个空白空间，长度为32

        /*清空buf1后面的缓冲区*/
        for (; pos < 32; pos++)
        {
            memset(buf[pos], '\0', sizeof(buf[pos]));
        }
        pos = argc - 1;
        
        /*恢复pass的链接，之前pass[pos]=0将使链接失效(中间一个为null)*/
        for (i = 0; i < 32; i++) //初始化指针数组pass，使第i个pass指针 指向buf的第i行。
            pass[i] = buf[i];

        char *c = buf[pos]; //新建指针c指向buf中的空白区域，此时buf[0]="echo" buf[1]="bye" buf[2]=NULL，c指向buf[2]

        /*for (i = 0; i < 32; i++)
        {
            printf("in while buf[i] = %s\n", buf[i]);
        }*/
        // printf("the buf2 = %s\n", buf2); //buf2此时为读到的用户键盘输入buf2 = hello too
        
        for (char *p = buf2; *p; p++)    //p指向buf2的开头，遍历buf2的内容
        {
            if (*p == ' ' || *p == '\n') //如果为' '或'\n'，则在c之后添加'\0' 相当于把buf2中的参数按照空格或换行符分开，存入buf[][]中的空白区域
            {
                *c = '\0'; //添加'\0'使buf[][]存储为字符串
                // printf("now buf[%d] = %s\n", pos, buf[pos]);
                pos++;
                c = buf[pos]; //c指向下一个空白区域
            }
            else
                *c++ = *p; //否则，复制p所指buf2中的字符到c所指的buf空白区域中。
        }
        memset(buf2, '\0', sizeof(buf2)); //清空buf2的缓冲区

        //printf("after loop the pos = %d\n", pos);

        pass[pos] = 0; //pass[pos]=0,这样exec()函数才知道pass参数在哪里结束，注意之后一定要恢复，否则中间一直有0
        
        /*调试信息*/
        /*
        for (i = 0; i < pos; i++)
        {
            printf("pass[%d] is %s\n", i, pass[i]);
        }
        for (i = 0; i < pos; i++)
        {
            printf("buf[%d] is %s\n", i, buf[i]);
        }
        */

        if (fork()) //创建子进程
        {
            wait(); //父进程等待子进程结束
        }
        else
            exec(pass[0], pass); //加载并执行文件，pass[0]中的参数为echo，pass为参数，所以执行echo bye hello too?
    }

    if (n < 0)
    {
        printf("xargs: read error\n");
        exit();
    }

    exit();
}