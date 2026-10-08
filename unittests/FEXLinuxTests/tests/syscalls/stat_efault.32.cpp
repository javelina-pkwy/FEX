#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <fcntl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

// Large enough for any of the stat and statfs structures.
alignas(8) static char Buffer[256];

#define CHECK_ERROR(Expr, ExpectedError) \
  do {                                   \
    long Result = (Expr);                \
    REQUIRE(Result == -1);               \
    CHECK(errno == (ExpectedError));     \
  } while (0)

static void CheckPath(long Syscall) {
  CHECK_ERROR(::syscall(Syscall, nullptr, Buffer), EFAULT);
  CHECK_ERROR(::syscall(Syscall, "/", nullptr), EFAULT);
  // The path is resolved before the result is written back.
  CHECK_ERROR(::syscall(Syscall, "/does/not/exist", nullptr), ENOENT);
}

static void CheckFD(long Syscall) {
  int FD = ::open("/dev/null", O_RDONLY);
  REQUIRE(FD != -1);
  CHECK_ERROR(::syscall(Syscall, FD, nullptr), EFAULT);
  // The fd is checked before the result is written back.
  CHECK_ERROR(::syscall(Syscall, -1, nullptr), EBADF);
  ::close(FD);
}

TEST_CASE("stat family returns EFAULT for null pointers") {
  CheckPath(SYS_stat);
  CheckPath(SYS_lstat);
  CheckFD(SYS_fstat);
  CheckPath(SYS_oldstat);
  CheckPath(SYS_oldlstat);
  CheckFD(SYS_oldfstat);
  CheckPath(SYS_stat64);
  CheckPath(SYS_lstat64);
  CheckFD(SYS_fstat64);
  CHECK_ERROR(::syscall(SYS_fstatat64, AT_FDCWD, nullptr, Buffer, 0), EFAULT);
  CHECK_ERROR(::syscall(SYS_fstatat64, AT_FDCWD, "/", nullptr, 0), EFAULT);
  CHECK_ERROR(::syscall(SYS_fstatat64, AT_FDCWD, "/does/not/exist", nullptr, 0), ENOENT);
}

TEST_CASE("statfs family returns EFAULT for null pointers") {
  CheckPath(SYS_statfs);
  CheckFD(SYS_fstatfs);
  CHECK_ERROR(::syscall(SYS_statfs64, nullptr, sizeof(struct statfs64), Buffer), EFAULT);
  CHECK_ERROR(::syscall(SYS_statfs64, "/", sizeof(struct statfs64), nullptr), EFAULT);
  CHECK_ERROR(::syscall(SYS_statfs64, "/does/not/exist", sizeof(struct statfs64), nullptr), ENOENT);
  int FD = ::open("/dev/null", O_RDONLY);
  REQUIRE(FD != -1);
  CHECK_ERROR(::syscall(SYS_fstatfs64, FD, sizeof(struct statfs64), nullptr), EFAULT);
  CHECK_ERROR(::syscall(SYS_fstatfs64, -1, sizeof(struct statfs64), nullptr), EBADF);
  ::close(FD);
}
