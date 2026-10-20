// Microbenchmarks for x86-64 `div r64` under FEX.
//
//   tput    independent 128/64 divides (RDX != 0) over a 4096 entry table
//   lat     dependent chain, bignum-by-word style: RDX = previous remainder
//   zero    independent 64/64 divides (RDX == 0), control for the fast path
//   tput10  independent 128/64 divides by 10 (RDX in 1..9)
//   lat10   dependent chain dividing by 10, RDX = previous remainder
//   u128fmt format unsigned __int128 values as decimal; libgcc's __udivti3/__umodti3
//           issue `div` with RDX != 0 for most of them
//
// Build: x86_64-linux-gnu-gcc -O2 -static -o ludiv_bench ludiv_bench.c
// Run:   FEX ./ludiv_bench <mode> <reps>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

#define N 4096

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) {
  uint64_t x = rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

static uint64_t His[N], Los[N], Divs[N];

static inline uint64_t div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t* r) {
  uint64_t q;
  __asm__("divq %4" : "=a"(q), "=d"(*r) : "a"(lo), "d"(hi), "r"(d));
  return q;
}

// Runtime radix so GCC can't strength-reduce the 128-bit divide into multiplies.
static volatile uint64_t Radix = 10;

static char* fmt_u128(u128 v, uint64_t radix, char* end) {
  *--end = 0;
  do {
    *--end = '0' + (unsigned)(v % radix);
    v /= radix;
  } while (v);
  return end;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s tput|lat|tput10|lat10|zero|u128fmt reps\n", argv[0]);
    return 1;
  }
  const char* mode = argv[1];
  uint64_t reps = strtoull(argv[2], NULL, 0);
  for (int i = 0; i < N; ++i) {
    uint64_t d = rng() >> (rng() & 31);
    d |= 2;
    Divs[i] = d;
    His[i] = rng() % d;
    if (His[i] == 0) {
      His[i] = 1;
    }
    Los[i] = rng();
  }

  uint64_t sum = 0;
  if (!strcmp(mode, "tput")) {
    for (uint64_t rep = 0; rep < reps; ++rep) {
      for (int i = 0; i < N; ++i) {
        uint64_t r;
        uint64_t q = div128(His[i], Los[i] ^ rep, Divs[i], &r);
        sum += q ^ r;
      }
    }
  } else if (!strcmp(mode, "lat")) {
    for (uint64_t rep = 0; rep < reps; ++rep) {
      uint64_t d = Divs[rep & (N - 1)] | (1ull << 40);
      uint64_t r = (rep + 1) % d;
      for (int i = 0; i < N; ++i) {
        uint64_t q = div128(r, Los[i], d, &r);
        sum += q;
      }
      sum += r;
    }
  } else if (!strcmp(mode, "tput10") || !strcmp(mode, "lat10")) {
    uint64_t ten = Radix;
    if (mode[0] == 't') {
      for (uint64_t rep = 0; rep < reps; ++rep) {
        for (int i = 0; i < N; ++i) {
          uint64_t r;
          uint64_t q = div128(1 + His[i] % 9, Los[i] ^ rep, ten, &r);
          sum += q ^ r;
        }
      }
    } else {
      for (uint64_t rep = 0; rep < reps; ++rep) {
        uint64_t r = 1 + rep % 9;
        for (int i = 0; i < N; ++i) {
          uint64_t q = div128(r, Los[i], ten, &r);
          sum += q;
        }
        sum += r;
      }
    }
  } else if (!strcmp(mode, "zero")) {
    for (uint64_t rep = 0; rep < reps; ++rep) {
      for (int i = 0; i < N; ++i) {
        uint64_t r;
        uint64_t q = div128(0, Los[i] ^ rep, Divs[i], &r);
        sum += q ^ r;
      }
    }
  } else if (!strcmp(mode, "u128fmt")) {
    char buf[160];
    uint64_t radix = Radix;
    for (uint64_t rep = 0; rep < reps; ++rep) {
      for (int i = 0; i < N; i += 4) {
        u128 v = ((u128)(His[i] ^ rep) << 64) | Los[i];
        char* s = fmt_u128(v, radix, buf + sizeof(buf));
        sum += (uint64_t)(buf + sizeof(buf) - s) + (uint8_t)s[0] + (uint8_t)s[5];
      }
    }
  } else {
    fprintf(stderr, "unknown mode %s\n", mode);
    return 1;
  }
  printf("%s %" PRIu64 " checksum %016" PRIx64 "\n", mode, reps, sum);
  return 0;
}
