#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// One case per process: argv[1] = path|buf, argv[2] = stat|lstat|statfs, argv[3] = none|unmapped
int main(int argc, char** argv) {
  void* Bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (strcmp(argv[3], "none") == 0) {
    mprotect(Bad, 4096, PROT_NONE);
  } else {
    munmap(Bad, 4096);
  }
  static char Good[256];
  int BadPath = strcmp(argv[1], "path") == 0;
  const char* Path = BadPath ? (const char*)Bad : "/";
  void* Buf = BadPath ? Good : Bad;
  long Nr = strcmp(argv[2], "stat") == 0 ? SYS_stat : strcmp(argv[2], "lstat") == 0 ? SYS_lstat : SYS_statfs;
  errno = 0;
  long Result = syscall(Nr, Path, Buf);
  printf("returned %ld %s\n", Result, Result < 0 ? strerrorname_np(errno) : "");
  return 0;
}
