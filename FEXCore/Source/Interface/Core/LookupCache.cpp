// SPDX-License-Identifier: MIT
/*
$info$
tags: glue|block-database
desc: Stores information about blocks, and provides C++ implementations to lookup the blocks
$end_info$
*/

#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"

namespace FEXCore {
GuestToHostMap::GuestToHostMap()
  : BlockLinks_mbr {"FEXMem_BlockLinks"} {
  BlockLinks_pma = fextl::make_unique<std::pmr::polymorphic_allocator<std::byte>>(&BlockLinks_mbr);
  // Setup our PMR map.
  BlockLinks = BlockLinks_pma->new_object<BlockLinksMapType>();
}

LookupCache::LookupCache(FEXCore::Context::ContextImpl* CTX)
  : ctx {CTX} {

  // Reserve for L2's maximum possible size (it grows/shrinks dynamically within this reservation, see
  // UpdateDynamicL2Stats) plus extra padding so L1Pointer can be rounded up to a FIXED_L1_SIZE-aligned
  // address below.
  TotalCacheSize = ctx->Config.VirtualMemSize / FEXCore::Utils::FEX_PAGE_SIZE * 8 + MAX_CODE_SIZE + MAX_L1_SIZE + FIXED_L1_SIZE;

  // Block cache ends up looking like this
  // PageMemoryMap[VirtualMemoryRegion >> 12]
  //       |
  //       v
  // PageMemory[Memory & (VIRTUAL_PAGE_SIZE - 1)]
  //       |
  //       v
  // Pointer to Code
  //
  // Allocate a region of memory that we can use to back our block pointers
  // We need one pointer per page of virtual memory
  // At 64GB of virtual memory this will allocate 128MB of virtual memory space
  PagePointer = reinterpret_cast<uintptr_t>(FEXCore::Allocator::VirtualAlloc(TotalCacheSize, false, false));
  LOGMAN_THROW_A_FMT(PagePointer != -1ULL, "Failed to allocate PagePointer");

  // Disable THP on the Lookup cache.
  FEXCore::Allocator::VirtualTHPControl(reinterpret_cast<const void*>(PagePointer), TotalCacheSize, FEXCore::Allocator::THPControl::Disable);

  FEXCore::Allocator::VirtualName("FEXMem_Lookup", reinterpret_cast<void*>(PagePointer),
                                  ctx->Config.VirtualMemSize / FEXCore::Utils::FEX_PAGE_SIZE * 8 + MAX_CODE_SIZE);
  CTX->SyscallHandler->MarkOvercommitRange(PagePointer, TotalCacheSize);

  // Allocate our memory backing our pages
  // We need 32KB per guest page (One pointer per byte)
  // XXX: We can drop down to 16KB if we store 4byte offsets from the code base
  // We currently limit to 128MB of real memory for caching for the total cache size.
  // Can end up being inefficient if we compile a small number of blocks per page
  PageMemory = PagePointer + ctx->Config.VirtualMemSize / FEXCore::Utils::FEX_PAGE_SIZE * 8;

  // L1 Cache
  // Placed after L2's *maximum* possible size (not its current, smaller starting size) so that L2
  // growing at runtime never runs into L1's region. Round up to a FIXED_L1_SIZE-aligned address so
  // that REG_L1_POINTER-relative JIT codegen can rely on the table's alignment (the padding for this
  // rounding is reserved above in TotalCacheSize).
  L1Pointer = FEXCore::AlignUp(PageMemory + MAX_CODE_SIZE, FIXED_L1_SIZE);
  FEXCore::Allocator::VirtualName("FEXMem_Lookup_L1", reinterpret_cast<void*>(L1Pointer), MAX_L1_SIZE);

  VirtualMemSize = ctx->Config.VirtualMemSize;

  if (DynamicL1Cache()) {
    // Legacy behaviour: start L1 at its minimum size and let UpdateDynamicL1Stats grow it. L2 is held
    // at the fixed legacy budget in this mode, since the heuristic is driving L1 instead.
    CurrentL1Entries = MIN_L1_ENTRIES;
    L1PointerMask = MIN_L1_ENTRIES - 1;
    CurrentCodeSize = LEGACY_CODE_SIZE;
  } else {
    // L1 is pinned to FIXED_L1_ENTRIES, so the mask never changes after this. It mirrors
    // FIXED_L1_INDEX_MASK, which JIT codegen bakes in directly as an immediate when pinning is on.
    // L2 starts at its minimum and is grown by UpdateDynamicL2Stats instead.
    CurrentL1Entries = FIXED_L1_ENTRIES;
    L1PointerMask = FIXED_L1_INDEX_MASK;
    CurrentCodeSize = MIN_CODE_SIZE;
  }
}

LookupCache::~LookupCache() {
  if (getenv("FEX_L1L2STATS")) {
    const uint64_t Lookups = Diag.L1Hits + Diag.L1Misses;
    const double MissPct = Lookups ? static_cast<double>(Diag.L1Misses) / static_cast<double>(Lookups) * 100.0 : 0.0;
    LogMan::Msg::IFmt("[L1L2STATS] mode={} L1={}KB/{}ent lookups={} L1miss={} ({:.2f}%) L2clears={} L2grow={} "
                      "L2shrink={} L1resize={} finalL2={}KB/{}pages",
                      DynamicL1Cache() ? "dynamicL1" : (ctx->UsesPinnedL1Pointer() ? "fixedL1+pinned" : "fixedL1"),
                      (L1PointerMask + 1) * sizeof(LookupCacheEntry) / 1024, L1PointerMask + 1, Lookups, Diag.L1Misses, MissPct,
                      Diag.L2Clears, Diag.L2Grows, Diag.L2Shrinks, Diag.L1Resizes, CurrentCodeSize / 1024, CurrentCodeSize / SIZE_PER_PAGE);
  }

  FEXCore::Allocator::VirtualFree(reinterpret_cast<void*>(PagePointer), TotalCacheSize);
  ctx->SyscallHandler->UnmarkOvercommitRange(PagePointer, TotalCacheSize);

  // No need to free BlockLinks map.
  // These will get freed when their memory allocators are deallocated.
}

void LookupCache::ClearL2Cache(const FEXCore::LookupCacheBaseLockToken& lk) {
  // Clear out the page memory
  // PagePointer and PageMemory are sequential with each other. Clear both at once.
  // Always decommits up through MAX_CODE_SIZE (not just CurrentCodeSize) so nothing stale lingers
  // from before a prior shrink -- CurrentCodeSize itself is left untouched by a clear, only the data.
  FEXCore::Allocator::VirtualDontNeed(reinterpret_cast<void*>(PagePointer),
                                      ctx->Config.VirtualMemSize / FEXCore::Utils::FEX_PAGE_SIZE * 8 + MAX_CODE_SIZE, false);
  AllocateOffset = 0;
  FEX_L1L2_DIAG_INC(Diag.L2Clears);
}

void LookupCache::ClearThreadLocalCaches(const LookupCacheWriteLockToken&) {
  // TODO: Preserve code cache entries?
  // Clear L1 and L2 by clearing the full cache.
  FEXCore::Allocator::VirtualDontNeed(reinterpret_cast<void*>(PagePointer), TotalCacheSize, false);

  // TODO: Rename this member to avoid confusion with code caching
  CachedCodePages.clear();
}

void LookupCache::ClearCache(const LookupCacheWriteLockToken& lk) {
  // Clear L1 and L2 by clearing the full cache.
  ClearThreadLocalCaches(lk);
  Shared->ClearCache(lk);
}

void GuestToHostMap::ClearCache(const LookupCacheWriteLockToken&) {
  // Allocate a new pointer from the BlockLinks pma again.
  BlockLinks = BlockLinks_pma->new_object<BlockLinksMapType>();
  // All code is gone, clear the block list
  BlockList.clear();
}

} // namespace FEXCore
