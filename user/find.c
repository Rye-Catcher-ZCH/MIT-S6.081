#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

/*这是一个更新buf的函数，将path所指的文件路径添加到buf中
  eg：/a/ -> /a/name
  注意当检测到path对应的是一个文件时，才调用该函数。
  该函数先找到path的最后一个'/'，然后将'/'后面的文件名复制到buf中，
  之所以调用该函数，是因为文件名之后不用再加'/'
*/
char *fmtname(char *path)
{
  static char buf[DIRSIZ + 1];
  char *p;

  // Find first character after last slash.
  // 找到前一个'/'的后一个字符
  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if (strlen(p) >= DIRSIZ) //如果path>DIRSIZ，返回空白名
    return p;
  memmove(buf, p, strlen(p));                       //复制路径到'/'之后
  memset(buf + strlen(p), ' ', DIRSIZ - strlen(p)); //复制完成的路径之后DRISIZ-strlen(p)的区域置为空字符。
  return buf;
}

// Regexp matcher from Kernighan & Pike,
// The Practice of Programming, Chapter 9.

int matchhere(char *, char *);
int matchstar(int, char *, char *);

/*之前的match函数有错误，导致只要文件名中有b就会打印
  修改后，之后文件名完全匹配才会打印
*/
int match(char *re, char *text)
{
  // printf("in match re = %s\n", re);
  // printf("in match text = %s\n", text);
  if (re[0] == '^')
    return matchhere(re + 1, text);
  /*do
  { // must look at empty string
    if (matchhere(re, text))  //匹配返回1
      return 1;
  } while (*text++ != '\0');*/
  // printf("the second char of text = %c\n",text[1]);
  if (matchhere(re, text))
    return 1;
  return 0; //否则，返回0
}

// matchhere: search for re at beginning of text
int matchhere(char *re, char *text)
{
  if (re[0] == '\0' && text[0] == ' ')  //re和text完全匹配才行，text有剩不行,eg:b和bigfile不匹配,text不满的地方是' '
    return 1;
  if (re[1] == '*')
    return matchstar(re[0], re + 2, text);
  if (re[0] == '$' && re[1] == '\0')
    return *text == '\0';
  if (*text != '\0' && (re[0] == '.' || re[0] == *text))
    return matchhere(re + 1, text + 1);
  return 0;
}

// matchstar: search for c*re at beginning of text
int matchstar(int c, char *re, char *text)
{
  do
  { // a * matches zero or more instances
    if (matchhere(re, text))
      return 1;
  } while (*text != '\0' && (*text++ == c || c == '.'));
  return 0;
}

void find(char *path, char *re)
{
  char buf[512], *p;
  // char *tmp;
  int fd;
  struct dirent de; //类似于linux里面f_path中的dentry结构体，定义在fs.h中，最大文件名为14(DIRSIZ)
  struct stat st;   //stat结构体存储d_name对应的文件的详细信息，其定义在stat.h中

  if ((fd = open(path, 0)) < 0) //open(filename, flags); 打开文件并指定读写模式
  {
    fprintf(2, "find: cannot open %s\n", path); //如果打开失败，向屏幕(文件描述符为2)输出错误提示信息
    return;
  }

  if (fstat(fd, &st) < 0) //fstat(fd, &stat结构体); 返回文件信息，返回的文件信息存储在st结构体中，如果返回失败，输出错误提示信息
  {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type) //判断当前path的所指对象的类型
  {
  case T_FILE: //如果是一个文件
    // printf("re = %s\n", re);
    // tmp = fmtname(path);
    // printf("t file buf = %s\n", tmp);
    if (match(re, fmtname(path)))     //并且该文件的路径与传入的re匹配，注意这里fmtname更新buf存储的路径
      printf("%s\n", path); //打印文件路径
    break;

  case T_DIR:                                        //如果是一个路径
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) //如果当前路径+最大路径长度大于缓冲区大小，更多信息见文档chapter 6目录层
    {
      printf("find: path too long\n"); //路径太长
      break;
    }
    strcpy(buf, path);                              //将路径复制到buf中
    p = buf + strlen(buf);                          //取得当前路径字符串的尾部地址，为buf的首地址+当前buf的长度。
    *p++ = '/';                                     //加上'/'字符
    while (read(fd, &de, sizeof(de)) == sizeof(de)) //读取文件描述符对应的dirent结构体到de中
    {
      if (de.inum == 0) //i节点编号是0的条目都是可用的，读到后，直接跳过
        continue;
      //printf("de.name = %s\n",de.name);
      memmove(p, de.name, DIRSIZ); //将de.name（一个字符串数组的首地址）所指区域的DIRSIZ个子节拷贝到p所指区域(buf中)
      //printf("buf = %s\n",buf);
      p[DIRSIZ] = 0;          //最后一个字符变为0？
      if (stat(buf, &st) < 0) //如果获取buf路径所对应的文件信息失败
      {
        printf("find: cannot stat %s\n", buf);
        continue;
      }

      //不要递归到"."和".."
      if (strlen(de.name) == 1 && de.name[0] == '.')
        continue;
      if (strlen(de.name) == 2 && de.name[0] == '.' && de.name[1] == '.')
        continue;

      find(buf, re);
    }
    break;
  }
  close(fd);
}

int main(int argc, char *argv[])
{
  if (argc <= 2)
    fprintf(2, "find: not enough params provided");
  //printf("re = %s\n",argv[2]);
  find(argv[1], argv[2]);

  exit();
}