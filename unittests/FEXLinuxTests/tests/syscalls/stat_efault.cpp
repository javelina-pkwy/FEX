#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <fcntl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

// Large enough for any of the stat and statfs structures.
alignas(8) static char Buffer[256];

static void CheckError(long Result, int ExpectedError) {
  REQUIRE(Result == -1);
  CHECK(errno == ExpectedError);
}

template<typename... Args>
static void CheckPath(long Syscall, Args... Extra) {
  CheckError(::syscall(Syscall, nullptr, Extra..., Buffer), EFAULT);
  CheckError(::syscall(Syscall, "/", Extra..., nullptr), EFAULT);
  // The path is resolved before the result is written back.
  CheckError(::syscall(Syscall, "/does/not/exist", Extra..., nullptr), ENOENT);
}

template<typename... Args>
static void CheckFD(long Syscall, Args... Extra) {
  int FD = ::open("/dev/null", O_RDONLY);
  REQUIRE(FD != -1);
  CheckError(::syscall(Syscall, FD, Extra..., nullptr), EFAULT);
  // The fd is checked before the result is written back.
  CheckError(::syscall(Syscall, -1, Extra..., nullptr), EBADF);
  ::close(FD);
}

TEST_CASE("stat family returns EFAULT for null pointers") {
  CheckPath(SYS_stat);
  CheckPath(SYS_lstat);
  CheckFD(SYS_fstat);
#ifdef __x86_64__
  CheckError(::syscall(SYS_newfstatat, AT_FDCWD, nullptr, Buffer, 0), EFAULT);
  CheckError(::syscall(SYS_newfstatat, AT_FDCWD, "/", nullptr, 0), EFAULT);
  CheckError(::syscall(SYS_newfstatat, AT_FDCWD, "/does/not/exist", nullptr, 0), ENOENT);
#else
  CheckPath(SYS_oldstat);
  CheckPath(SYS_oldlstat);
  CheckFD(SYS_oldfstat);
  CheckPath(SYS_stat64);
  CheckPath(SYS_lstat64);
  CheckFD(SYS_fstat64);
  CheckError(::syscall(SYS_fstatat64, AT_FDCWD, nullptr, Buffer, 0), EFAULT);
  CheckError(::syscall(SYS_fstatat64, AT_FDCWD, "/", nullptr, 0), EFAULT);
  CheckError(::syscall(SYS_fstatat64, AT_FDCWD, "/does/not/exist", nullptr, 0), ENOENT);
#endif
}

TEST_CASE("statfs family returns EFAULT for null pointers") {
  CheckPath(SYS_statfs);
  CheckFD(SYS_fstatfs);
#ifndef __x86_64__
  CheckPath(SYS_statfs64, sizeof(struct statfs64));
  CheckFD(SYS_fstatfs64, sizeof(struct statfs64));
#endif
}
