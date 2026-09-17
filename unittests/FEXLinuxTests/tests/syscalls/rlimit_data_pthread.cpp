#include <catch2/catch_test_macros.hpp>

#include <pthread.h>
#include <sys/resource.h>

static void* ThreadFunc(void*) {
  return nullptr;
}

// Chromium's sandbox lowers RLIMIT_DATA to 16GB and then spawns threads.
// The kernel counts every private writable mapping towards this limit, so
// FEX's own reservations must not push the process over it.
TEST_CASE("pthread_create after lowering RLIMIT_DATA") {
  rlimit64 Limit {
    .rlim_cur = 16ULL * 1024 * 1024 * 1024,
    .rlim_max = 16ULL * 1024 * 1024 * 1024,
  };
  REQUIRE(setrlimit64(RLIMIT_DATA, &Limit) == 0);

  pthread_t Thread;
  REQUIRE(pthread_create(&Thread, nullptr, ThreadFunc, nullptr) == 0);
  CHECK(pthread_join(Thread, nullptr) == 0);
}
