// Measures the core clock: times dependent add chains (1 cycle each) and imul chains (3 cycles each on Zen 3).
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
int main(void) {
  const long iters = 20000000; // x 100 instructions
  uint64_t a = 1, one = 1;
  for (int r = 0; r < 3; ++r) {
    double t0 = now();
    for (long i = 0; i < iters; ++i) __asm__ volatile(".rept 100\n add %0, %1\n .endr" : "+r"(a) : "r"(one));
    double t1 = now();
    for (long i = 0; i < iters; ++i) __asm__ volatile(".rept 100\n imul %0, %1\n .endr" : "+r"(a) : "r"(one));
    double t2 = now();
    printf("add chain %.3f GHz | imul chain %.3f GHz (x3 latency)\n", iters * 100 / (t1 - t0) / 1e9, iters * 300 / (t2 - t1) / 1e9);
  }
  return (int)(a & 1) - 1;
}
