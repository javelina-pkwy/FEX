// Dumb 128/64 divide microbenchmark: random dividends and divisors, timed in the guest.
// Every divide has RDX != 0 and RDX < divisor, so x86 never faults.
//
// Build: x86_64-linux-gnu-gcc -O2 -static -o dumb_div dumb_div.c
// Run:   FEX ./dumb_div [divides per pass] [passes]
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 0x2545F4914F6CDD1Dull;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
  size_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 1 << 18;
  int passes = argc > 2 ? atoi(argv[2]) : 200;

  uint64_t* hi = malloc(n * sizeof(uint64_t));
  uint64_t* lo = malloc(n * sizeof(uint64_t));
  uint64_t* d = malloc(n * sizeof(uint64_t));
  for (size_t i = 0; i < n; ++i) {
    d[i] = rng() | 2;
    hi[i] = rng() % d[i];
    if (hi[i] == 0) {
      hi[i] = 1;
    }
    lo[i] = rng();
  }

  uint64_t sum = 0;
  double start = now();
  for (int p = 0; p < passes; ++p) {
    for (size_t i = 0; i < n; ++i) {
      uint64_t q, r;
      __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo[i]), "d"(hi[i]), "r"(d[i]));
      sum += q ^ r;
    }
  }
  double elapsed = now() - start;

  double divides = (double)n * passes;
  printf("%.0f divides in %.3f s: %.2f ns/divide (checksum %016" PRIx64 ")\n", divides, elapsed, elapsed * 1e9 / divides, sum);
  return 0;
}
