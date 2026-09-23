# TODO

## Single-instruction SVE gather when the mask is the `vpcmpeqd x,x,x` all-ones idiom

Geekbench 6 Horizon Detection (block `geekbench_avx2+0x6611e0`, ~16% of the workload) and Photo Filter, and
compiler-emitted unmasked gathers in general, build the gather mask with `vpcmpeqd ymmM, ymmM, ymmM` immediately
before `vpgatherdd ymmD, [base + ymmI*4], ymmM`. FEX's SVE path (`VLoadVectorGatherMasked`) still pays the full
masked sequence per 128-bit half:

```
cmplt p0.s, p6/z, zMask.s, #0          ; mask sign bits -> predicate
ld1w  {zTmp.s}, p0/z, [xBase, zIdx.s, sxtw #2]
sel   zDst.s, p0, zTmp.s, zOld.s       ; merge with the old destination (mov zDst,p0/m when Dst == Old)
```

Plan:

1. `IsVectorAllOnes(LastXMMDef(mask))` in the AVX-128 dispatcher: true when the mask register's last definition
   (tracked by `RegLastDef`, which survives the per-instruction SRA flush) is a `VCMPEQ`/`VFCMPEQ` whose two source
   Refs are the same register.
2. An unmasked `VLoadVectorGather` IR op (no `Incoming`, no `Mask`) lowered to one `ld1{b,h,w,d}` with the all-true
   governing predicate straight into the destination. ASIMD fallback: reuse `Emulate128BitGather` with a synthesized
   all-ones mask. Same for the `QPS` variant (`vpgatherqd` / `vgatherqps`).
3. In `AVX128_VPGATHER`, check the low and high mask halves independently and use the unmasked op per half.
   The mask-register clear after the gather stays (x86 zeroes the mask register).
4. Tests: `ctest -R gather`, plus a differential test with all-ones masks, partial masks, and compare-of-different-
   registers masks (only the self-compare may take the shortcut).

Expected: 2 of 3 instructions per half-gather removed (about 6% of the Horizon Detection resample loop, and a shorter
gather dependency chain). Optional follow-up: a dead-definition pass over `RegLastDef` could also drop the `cmeq`
that builds the mask and the zeroing `mov`, since the mask is dead once the gather consumes it.
