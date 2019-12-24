#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
//#include <stat.h>
//#include <user.h>

int main(int argc, char *argv[])
{
  int p;
  if(argc < 2 || argc > 2)
  {
    printf("error! only need one parameter!\n");
    exit(-1);
  }
  p = atoi(argv[1]);
  printf("wait for %d seconds\n",p);
  sleep(p*10);
  printf("wait is over\n");
  exit(0);
}

//sleep如何控制在1s以内?