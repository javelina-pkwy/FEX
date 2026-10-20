// Random 128/64 divides through IDIV r64 and DIV r64, RDX not a sign/zero extension of RAX, quotient always fits.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1Dull; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
int main(int argc, char** argv) {
  int sign = !strcmp(argv[1], "idiv");
  size_t n = 1 << 16; int passes = 200;
  uint64_t *hi = malloc(n * 8), *lo = malloc(n * 8), *d = malloc(n * 8);
  for (size_t i = 0; i < n; ++i) {
    d[i] = (rng() >> 2) | (1ull << 40);      // positive, < 2^62
    hi[i] = 1 + rng() % ((d[i] >> 1) - 1);    // 0 < hi < d/2: not an extension of lo, quotient < 2^63
    lo[i] = rng();
  }
  uint64_t sum = 0; double t0 = now();
  for (int p = 0; p < passes; ++p)
    for (size_t i = 0; i < n; ++i) {
      uint64_t q, r;
      if (sign) __asm__ volatile("idivq %4" : "=a"(q), "=d"(r) : "a"(lo[i]), "d"(hi[i]), "r"(d[i]));
      else      __asm__ volatile("divq %4"  : "=a"(q), "=d"(r) : "a"(lo[i]), "d"(hi[i]), "r"(d[i]));
      sum += q ^ r;
    }
  double dt = now() - t0;
  printf("%s %.2f ns/divide (checksum %016" PRIx64 ")\n", argv[1], dt * 1e9 / ((double)n * passes), sum);
}
