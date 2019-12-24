#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
int main()
{
  int parent_fd[2];
  int child_fd[2];
  int child_pid;
  int parent_pid;
  char child_read[100];
  char parent_read[100];
  pipe(parent_fd);
  pipe(child_fd);
  if(fork() == 0)
  {
    //close(0);
    child_pid = getpid();
    if(read(parent_fd[0], child_read, 10) > 1)
    {
      write(child_fd[1], "pong\n", 5);
    }
    printf("%d: received %s\n",child_pid, child_read);
    close(parent_fd[0]);
    close(parent_fd[1]);
    exit(0);
  }
  else
  {
    parent_pid = getpid();
    write(parent_fd[1], "ping\n" ,5);
    read(child_fd[0], parent_read, 10);
    printf("%d: received %s",parent_pid, parent_read);
    close(child_fd[0]);
    close(child_fd[1]);
  }
  exit(0);
}
