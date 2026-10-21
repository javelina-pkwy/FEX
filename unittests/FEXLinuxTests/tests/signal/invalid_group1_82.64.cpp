#include "invalid_util.h"

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <stdint.h>

#ifndef REG_RIP
#define REG_RIP REG_EIP
#endif

// 0x82 is an alias of the 0x80 group 1 encoding in 32-bit mode only.
// In 64-bit mode it is an invalid opcode and must raise #UD, for every value of the ModRM reg field.

constexpr int EXPECTED_TRAPNO = 6;
constexpr int EXPECTED_ERR = 0;
constexpr int EXPECTED_SI_CODE = 2;
constexpr int EXPECTED_SIGNAL = SIGILL;

#define CONCAT(x, y) x##y
#define TestSymbols(num)                       \
  extern "C" uint64_t CONCAT(TestBegin_, num); \
  extern "C" uint64_t CONCAT(TestEnd_, num);

// The bytes are emitted as data so that the assembler does not reject the encoding.
#define TEST(num, name, bytes)                                                                                  \
  TestSymbols(num);                                                                                             \
  TEST_CASE("Signals: Invalid 0x82 " name) {                                                                    \
    struct sigaction act {};                                                                                    \
    act.sa_sigaction = CapturingHandler;                                                                        \
    act.sa_flags = SA_SIGINFO;                                                                                  \
    sigaction(SIGSEGV, &act, nullptr);                                                                          \
    sigaction(SIGTRAP, &act, nullptr);                                                                          \
    sigaction(SIGILL, &act, nullptr);                                                                           \
                                                                                                                \
    from_handler.reset();                                                                                       \
    capturing_handler_calls = 0;                                                                                \
    capturing_handler_skip = (unsigned long)&CONCAT(TestEnd_, num) - (unsigned long)&CONCAT(TestBegin_, num);   \
    const unsigned long EXPECTED_RIP = (unsigned long)&CONCAT(TestBegin_, num);                                 \
    __asm volatile("TestBegin_" #num ":\n.byte " bytes "\nTestEnd_" #num ":" ::: "memory");                    \
                                                                                                                \
    REQUIRE(from_handler.has_value());                                                                          \
    CHECK(capturing_handler_calls == 1);                                                                        \
    CHECK(from_handler->mctx.gregs[REG_RIP] == EXPECTED_RIP);                                                   \
    CHECK(from_handler->mctx.gregs[REG_TRAPNO] == EXPECTED_TRAPNO);                                             \
    CHECK(from_handler->mctx.gregs[REG_ERR] == EXPECTED_ERR);                                                   \
    CHECK(from_handler->si_code == EXPECTED_SI_CODE);                                                           \
    CHECK(from_handler->signal == EXPECTED_SIGNAL);                                                             \
  }

// Memory form: <op> byte [rax], 0x61. The fault is raised at decode, so [rax] is never accessed.
TEST(0, "/0 add byte [rax], imm8", "0x82, 0x00, 0x61");
TEST(1, "/1 or byte [rax], imm8", "0x82, 0x08, 0x61");
TEST(2, "/2 adc byte [rax], imm8", "0x82, 0x10, 0x61");
TEST(3, "/3 sbb byte [rax], imm8", "0x82, 0x18, 0x61");
TEST(4, "/4 and byte [rax], imm8", "0x82, 0x20, 0x61");
TEST(5, "/5 sub byte [rax], imm8", "0x82, 0x28, 0x61");
TEST(6, "/6 xor byte [rax], imm8", "0x82, 0x30, 0x61");
TEST(7, "/7 cmp byte [rax], imm8", "0x82, 0x38, 0x61");

// Locked memory form. CMP does not accept LOCK, which is also #UD.
TEST(8, "/0 lock add byte [rax], imm8", "0xf0, 0x82, 0x00, 0x61");
TEST(9, "/1 lock or byte [rax], imm8", "0xf0, 0x82, 0x08, 0x61");
TEST(10, "/2 lock adc byte [rax], imm8", "0xf0, 0x82, 0x10, 0x61");
TEST(11, "/3 lock sbb byte [rax], imm8", "0xf0, 0x82, 0x18, 0x61");
TEST(12, "/4 lock and byte [rax], imm8", "0xf0, 0x82, 0x20, 0x61");
TEST(13, "/5 lock sub byte [rax], imm8", "0xf0, 0x82, 0x28, 0x61");
TEST(14, "/6 lock xor byte [rax], imm8", "0xf0, 0x82, 0x30, 0x61");
TEST(15, "/7 lock cmp byte [rax], imm8", "0xf0, 0x82, 0x38, 0x61");

// Register form: <op> al, 0x61
TEST(16, "/0 add al, imm8", "0x82, 0xc0, 0x61");
TEST(17, "/1 or al, imm8", "0x82, 0xc8, 0x61");
TEST(18, "/2 adc al, imm8", "0x82, 0xd0, 0x61");
TEST(19, "/3 sbb al, imm8", "0x82, 0xd8, 0x61");
TEST(20, "/4 and al, imm8", "0x82, 0xe0, 0x61");
TEST(21, "/5 sub al, imm8", "0x82, 0xe8, 0x61");
TEST(22, "/6 xor al, imm8", "0x82, 0xf0, 0x61");
TEST(23, "/7 cmp al, imm8", "0x82, 0xf8, 0x61");
