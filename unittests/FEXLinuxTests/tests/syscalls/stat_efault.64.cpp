#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <fcntl.h>
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
}

static void CheckFD(long Syscall) {
  int FD = ::open("/dev/null", O_RDONLY);
  REQUIRE(FD != -1);
  CHECK_ERROR(::syscall(Syscall, FD, nullptr), EFAULT);
  ::close(FD);
}

TEST_CASE("stat family returns EFAULT for null pointers") {
  CheckPath(SYS_stat);
  CheckPath(SYS_lstat);
  CheckFD(SYS_fstat);
  CHECK_ERROR(::syscall(SYS_newfstatat, AT_FDCWD, nullptr, Buffer, 0), EFAULT);
  CHECK_ERROR(::syscall(SYS_newfstatat, AT_FDCWD, "/", nullptr, 0), EFAULT);
}

TEST_CASE("statfs family returns EFAULT for null pointers") {
  CheckPath(SYS_statfs);
  CheckFD(SYS_fstatfs);
}
