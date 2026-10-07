#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static char Buffer[256];

static void Report(const char* Name, long Result) {
  printf("%-52s -> %s\n", Name, Result < 0 ? strerrorname_np(errno) : "0 (success)");
}
#define T(Name, ...) do { errno = 0; Report(Name, syscall(__VA_ARGS__)); } while (0)

int main() {
  int FD = open("/dev/null", O_RDONLY);
  T("stat(\"/does/not/exist\", NULL)", SYS_stat, "/does/not/exist", NULL);
  T("fstat(-1, NULL)", SYS_fstat, -1, NULL);
#ifdef __x86_64__
  T("newfstatat(fd, NULL, &st, AT_EMPTY_PATH)", SYS_newfstatat, FD, NULL, Buffer, AT_EMPTY_PATH);
  T("newfstatat(fd, \"\", &st, AT_EMPTY_PATH)", SYS_newfstatat, FD, "", Buffer, AT_EMPTY_PATH);
  T("newfstatat(AT_FDCWD, NULL, &st, 0)", SYS_newfstatat, AT_FDCWD, NULL, Buffer, 0);
#else
  T("fstatat64(fd, NULL, &st, AT_EMPTY_PATH)", SYS_fstatat64, FD, NULL, Buffer, AT_EMPTY_PATH);
  T("fstatat64(fd, \"\", &st, AT_EMPTY_PATH)", SYS_fstatat64, FD, "", Buffer, AT_EMPTY_PATH);
  T("fstatat64(AT_FDCWD, NULL, &st, 0)", SYS_fstatat64, AT_FDCWD, NULL, Buffer, 0);
#endif
  return 0;
}
