#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <csetjmp>
#include <cstdint>

// An instruction that starts in executable memory and runs into an inaccessible page faults with
// si_addr pointing at the start of that page. Handlers that map code in on demand rely on this:
// they make the page at si_addr accessible and resume, which must then execute the instruction.
// If the instruction address is reported instead, the handler fixes up the wrong page and the
// instruction faults forever.

static jmp_buf LongJump {};
static long PageSize {};
static volatile int FaultCount {};
static volatile uintptr_t FaultAddress {};

constexpr int MaxFaults = 4;

static void SignalHandler(int, siginfo_t* Info, void*) {
  FaultAddress = reinterpret_cast<uintptr_t>(Info->si_addr);
  if (++FaultCount > MaxFaults) {
    longjmp(LongJump, 1);
  }

  const auto Page = FaultAddress & ~static_cast<uintptr_t>(PageSize - 1);
  mprotect(reinterpret_cast<void*>(Page), PageSize, PROT_READ | PROT_EXEC);
}

TEST_CASE("Signals: Resume after partial instruction fetch fault") {
  PageSize = sysconf(_SC_PAGESIZE);
  REQUIRE(PageSize > 0);

  struct sigaction Act {};
  Act.sa_sigaction = SignalHandler;
  Act.sa_flags = SA_SIGINFO;
  REQUIRE(sigaction(SIGSEGV, &Act, nullptr) == 0);

  auto Ptr = static_cast<uint8_t*>(mmap(nullptr, PageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  REQUIRE(Ptr != MAP_FAILED);

  // mov eax, 0x42424242; ret
  // Starts two bytes before the page boundary, so the immediate is in the second page.
  auto Code = Ptr + PageSize - 2;
  const uint8_t Bytes[] = {0xb8, 0x42, 0x42, 0x42, 0x42, 0xc3};
  for (size_t i = 0; i < sizeof(Bytes); ++i) {
    Code[i] = Bytes[i];
  }

  REQUIRE(mprotect(Ptr, PageSize, PROT_READ | PROT_EXEC) == 0);
  REQUIRE(mprotect(Ptr + PageSize, PageSize, PROT_NONE) == 0);

  using FuncPtr = uint32_t (*)();
  auto Func = reinterpret_cast<FuncPtr>(Code);

  uint32_t Result {};
  if (setjmp(LongJump) == 0) {
    Result = Func();
  }

  CHECK(FaultAddress == reinterpret_cast<uintptr_t>(Ptr + PageSize));
  CHECK(FaultCount == 1);
  CHECK(Result == 0x42424242);

  REQUIRE(munmap(Ptr, PageSize * 2) == 0);
}
