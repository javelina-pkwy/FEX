#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// One case per process: argv[1] = stat|fstat, argv[2] = ro|none
int main(int argc, char** argv) {
  int Prot = strcmp(argv[2], "ro") == 0 ? PROT_READ : PROT_NONE;
  void* Buf = mmap(NULL, 4096, Prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  long Result;
  errno = 0;
  if (strcmp(argv[1], "stat") == 0) {
    Result = syscall(SYS_stat, "/", Buf);
  } else {
    int FD = open("/dev/null", O_RDONLY);
    Result = syscall(SYS_fstat, FD, Buf);
  }
  printf("returned %ld %s\n", Result, Result < 0 ? strerrorname_np(errno) : "");
  return 0;
}
