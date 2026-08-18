// SPDX-License-Identifier: MIT
#pragma once
#include "Interface/Context/Context.h"
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/SHMStats.h>
#include <FEXCore/Utils/WritePriorityMutex.h>

#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/memory_resource.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/robin_set.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/fextl/memory_resource.h>

#include <cstdint>
#include <span>
#include <stddef.h>
#include <utility>
#include <mutex>

namespace FEXCore {
struct LookupCacheBaseLockToken {
protected:
  // Protected constructor - only derived classes can construct
  LookupCacheBaseLockToken() = default;
};

struct LookupCacheWriteLockToken : public LookupCacheBaseLockToken {
private:
  // Only constructible by GuestToHostMap
  friend struct GuestToHostMap;
  LookupCacheWriteLockToken(FEXCore::Utils::WritePriorityMutex::Mutex& Mutex)
    : Lock {Mutex} {}
  std::lock_guard<FEXCore::Utils::WritePriorityMutex::Mutex> Lock;
};

struct LookupCacheReadLockToken : public LookupCacheBaseLockToken {
private:
  // Only constructible by GuestToHostMap
  friend struct GuestToHostMap;
  LookupCacheReadLockToken(FEXCore::Utils::WritePriorityMutex::Mutex& Mutex)
    : Lock {Mutex} {}
  std::shared_lock<FEXCore::Utils::WritePriorityMutex::Mutex> Lock;
};

struct GuestToHostMap {
  FEXCore::Utils::WritePriorityMutex::Mutex Lock {};

  [[nodiscard]]
  LookupCacheWriteLockToken AcquireWriteLock() {
    return LookupCacheWriteLockToken {Lock};
  }

  [[nodiscard]]
  LookupCacheReadLockToken AcquireReadLock() {
    return LookupCacheReadLockToken {Lock};
  }

  struct BlockLinkTag {
    uint64_t GuestDestination;
    FEXCore::Context::ExitFunctionLinkData* HostLink;

    bool operator<(const BlockLinkTag& other) const {
      if (GuestDestination < other.GuestDestination) {
        return true;
      } else if (GuestDestination == other.GuestDestination) {
        return HostLink < other.HostLink;
      } else {
        return false;
      }
    }
  };

  // Use a monotonic buffer resource to allocate both the std::pmr::map and its members.
  // This allows us to quickly clear the block link map by clearing the monotonic allocator.
  // If we had allocated the block link map without the MBR, then clearing the map would require slowly
  // walking each block member and destructing objects.
  //
  // This makes `BlockLinks` look like a raw pointer that could memory leak, but since it is backed by the MBR, it won't.
  fextl::pmr::named_monotonic_page_buffer_resource BlockLinks_mbr;
  using BlockLinksMapType = std::pmr::map<BlockLinkTag, FEXCore::Context::BlockDelinkerFunc>;
  fextl::unique_ptr<std::pmr::polymorphic_allocator<std::byte>> BlockLinks_pma;
  BlockLinksMapType* BlockLinks;

  struct BlockEntry {
    uint64_t HostCode;
    fextl::vector<uint64_t> CodePages;
  };

  fextl::robin_map<uint64_t, BlockEntry> BlockList;

  fextl::map<uint64_t, fextl::vector<uint64_t>> CodePages;

  GuestToHostMap();

  // Adds to Guest -> Host code mapping
  const BlockEntry& AddBlockMapping(uint64_t Address, std::span<const uint64_t> CodePages, void* HostCode, const LookupCacheWriteLockToken&) {
    // This may replace an existing mapping
    // NOTE: Generally no previous entry should exist, however there is one exception:
    //       If the backend updates the active thread's CodeBuffer, the new associated LookupCache
    //       may already contain the block address. Since is comparatively rare, we'll just leak
    //       one of the two blocks in this case.
    return BlockList
      .insert_or_assign(Address, BlockEntry {(uintptr_t)HostCode, fextl::vector<uint64_t>(CodePages.begin(), CodePages.end())})
      .first->second;
  }

  const BlockEntry* FindBlock(uint64_t Address, const LookupCacheReadLockToken&) {
    auto HostCode = BlockList.find(Address);
    if (HostCode == BlockList.end()) {
      return nullptr;
    }
    return &HostCode->second;
  }

  bool Erase(uint64_t Address, const LookupCacheWriteLockToken&) {
    // Sever any links to this block
    auto lower = BlockLinks->lower_bound({Address, nullptr});
    auto upper = BlockLinks->upper_bound({Address, reinterpret_cast<FEXCore::Context::ExitFunctionLinkData*>(UINTPTR_MAX)});
    for (auto it = lower; it != upper; it = BlockLinks->erase(it)) {
      it->second(it->first.HostLink);
    }

    // Remove from BlockList
    return BlockList.erase(Address) != 0;
  }

  void InvalidateRange(uint64_t Start, uint64_t Length) {
    auto lk = AcquireWriteLock();

    auto lower = CodePages.lower_bound(Start >> 12);
    auto upper = CodePages.upper_bound((Start + Length - 1) >> 12);

    for (auto it = lower; it != upper; it++) {
      for (const auto& Entry : it->second) {
        Erase(Entry, lk);
      }
    }
    CodePages.erase(lower, upper);
  }

  void AddBlockLink(uint64_t GuestDestination, FEXCore::Context::ExitFunctionLinkData* HostLink,
                    const FEXCore::Context::BlockDelinkerFunc& delinker, const LookupCacheWriteLockToken&) {
    BlockLinks->insert({{GuestDestination, HostLink}, delinker});
  }

  bool AddBlockExecutableRange(const std::ranges::input_range auto& Addresses, uint64_t Start, uint64_t Length, const LookupCacheWriteLockToken&) {
    bool rv = false;

    for (auto CurrentPage = Start >> 12, EndPage = (Start + Length - 1) >> 12; CurrentPage <= EndPage; CurrentPage++) {
      auto& CodePage = CodePages[CurrentPage];
      rv |= CodePage.empty();
      CodePage.insert(CodePage.end(), Addresses.begin(), Addresses.end());
    }

    return rv;
  }

  void ClearCache(const LookupCacheWriteLockToken&);
};

class LookupCache {
public:
  struct LookupCacheEntry {
    uintptr_t HostCode;
    uintptr_t GuestCode;
  };

  // The L1 table is fixed at this size so the JIT can pin its base pointer in a dedicated register
  // (REG_L1_POINTER) and treat the index mask as a compile-time constant, rather than reloading
  // both from CpuStateFrame on every lookup. See BranchOps.cpp/Dispatcher.cpp L1 lookup codegen.
  constexpr static size_t FIXED_L1_SIZE = 512 * 1024; // Must be a power of 2.
  constexpr static size_t FIXED_L1_ENTRIES = FIXED_L1_SIZE / sizeof(LookupCacheEntry);
  constexpr static size_t FIXED_L1_INDEX_MASK = FIXED_L1_ENTRIES - 1;
  constexpr static size_t FIXED_L1_INDEX_BITS = FEXCore::ilog2(FIXED_L1_ENTRIES);

  LookupCache(FEXCore::Context::ContextImpl* CTX);
  ~LookupCache();

  // Swaps out the underlying GuestToHostMap and clears all associated caches.
  // This interface requires the previous CodeBuffer to be provided despite not using it. This ensures the shared write lock is still valid.
  void ChangeGuestToHostMapping([[maybe_unused]] CPU::CodeBuffer& Prev, GuestToHostMap& NewMap, const LookupCacheWriteLockToken& lk) {
    ClearThreadLocalCaches(lk);
    Shared = &NewMap;
  }

  uintptr_t FindBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t Address) {
    // Try L1, no lock needed
    auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Address & L1PointerMask];
    if (L1Entry.GuestCode == Address) {
      return L1Entry.HostCode;
    }

    // L2 and L3 need to be locked
    uintptr_t HostPtr {};
    {
      std::optional<FEXCore::SHMStats::AccumulationBlock<uint64_t>> LockTime(
        Thread->ThreadStats ? &Thread->ThreadStats->AccumulatedCacheReadLockTime : nullptr);
      auto lk = Shared->AcquireReadLock();
      LockTime.reset();

      if (!DisableL2Cache()) {
        // Try L2
        const auto PageIndex = (Address & (VirtualMemSize - 1)) >> 12;
        const auto PageOffset = Address & (0x0FFF);

        const auto Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
        auto LocalPagePointer = Pointers[PageIndex];

        // Do we a page pointer for this address?
        if (LocalPagePointer) {
          // Find there pointer for the address in the blocks
          auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);

          if (BlockPointers[PageOffset].GuestCode == Address) {
            L1Entry.GuestCode = Address;
            L1Entry.HostCode = BlockPointers[PageOffset].HostCode;
            HostPtr = L1Entry.HostCode;
          }
        }
      }

      if (!HostPtr) {
        // Try L3
        auto Entry = Shared->FindBlock(Address, lk);
        if (Entry) {
          CacheBlockMapping(Address, *Entry, false, lk);
          HostPtr = Entry->HostCode;
        }
      }
    }

    if (HostPtr && DynamicL1Cache()) {
      UpdateDynamicL2Stats();
    }

    FEXCORE_PROFILE_INSTANT_INCREMENT(Thread, AccumulatedCacheMissCount, 1);

    return HostPtr;
  }

  // Ported from the old dynamic L1 resize heuristic (L1 is now fixed-size so its base pointer/mask
  // can be pinned into JIT'd code). Same trigger condition and config knobs, but now grows/shrinks
  // L2's page-block backing budget (CurrentCodeSize) instead of L1's mask. Unlike L1, no JIT-visible
  // state needs updating here: L2 lookups walk PagePointer/PageMemory purely by non-null-pointer
  // checks, so growing/shrinking the allocation budget is invisible to already-compiled code.
  void UpdateDynamicL2Stats() {
    // If host pointer was found in L2 or L3, then add it to the counter.
    // Keeping track not L1 misses, but specifically L2/L3 hits.
    ++L2L3CacheHits;

    const auto CurrentTime = std::chrono::system_clock::now();
    const auto Period = CurrentTime - LastPeriod;
    if (Period >= SamplePeriod) {
      // If larger than the sample period then check if we need to increase L2 cache size.
      const double AveragePerSecond = static_cast<double>(L2L3CacheHits) /
                                      static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Period).count()) * 1000.0;

      if (AveragePerSecond >= DynamicL1CacheIncreaseCountHeuristic()) {
        if (CurrentCodeSize < MAX_CODE_SIZE) {
          CurrentCodeSize <<= 1;
        }
      } else if (AveragePerSecond < DynamicL1CacheDecreaseCountHeuristic()) {
        if (CurrentCodeSize > MIN_CODE_SIZE) {
          CurrentCodeSize >>= 1;

          // Madvise the page-block backing that we are dropping. Gives the memory back to the OS.
          // Any already-allocated page blocks beyond the new boundary become unreachable garbage:
          // AllocateBackingForPage's bounds check against CurrentCodeSize prevents new allocations
          // past it, and any stale PagePointer[] entries pointing past it now read back as zeroed
          // (GuestCode == 0), which just looks like an ordinary cache miss on next access.
          uint8_t* FirstZeroByte = reinterpret_cast<uint8_t*>(PageMemory) + CurrentCodeSize;
          size_t ZeroMemorySize = MAX_CODE_SIZE - CurrentCodeSize;
          FEXCore::Allocator::VirtualDontNeed(FirstZeroByte, ZeroMemorySize, false);
        }
      }

      // Update Last period to start again.
      LastPeriod = CurrentTime;
      L2L3CacheHits = 0;
    }
  }

  GuestToHostMap* Shared = nullptr;

  // Appends a list of Block {Address} to CodePages [Start, Start + Length)
  // Returns true if new pages are marked as containing code
  bool AddBlockExecutableRange(FEXCore::Core::InternalThreadState* Thread, auto& Addresses, uint64_t Start, uint64_t Length) {
    std::optional<FEXCore::SHMStats::AccumulationBlock<uint64_t>> LockTime(
      Thread->ThreadStats ? &Thread->ThreadStats->AccumulatedCacheWriteLockTime : nullptr);
    auto lk = Shared->AcquireWriteLock();
    LockTime.reset();

    return Shared->AddBlockExecutableRange(Addresses, Start, Length, lk);
  }

  // Adds to Guest -> Host code mapping
  void AddBlockMapping(FEXCore::Core::InternalThreadState* Thread, uint64_t Address, std::span<const uint64_t> CodePages, void* HostCode) {
    std::optional<FEXCore::SHMStats::AccumulationBlock<uint64_t>> LockTime(
      Thread->ThreadStats ? &Thread->ThreadStats->AccumulatedCacheWriteLockTime : nullptr);
    auto lk = Shared->AcquireWriteLock();
    LockTime.reset();

    const auto& Entry = Shared->AddBlockMapping(Address, CodePages, HostCode, lk);

    // There is no need to update L1 or L2, they will get updated on first lookup
    // However, adding to L1 here increases performance
    CacheBlockMapping(Address, Entry, true, lk);
  }

  // Invalidates L1/L2 for a given guest block
  void InvalidateCache(uint64_t Address, const LookupCacheWriteLockToken& lk) {
    // Do L1
    auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Address & L1PointerMask];
    if (L1Entry.GuestCode == Address) {
      L1Entry.GuestCode = 0;
      // Leave L1Entry.HostCode as is, so that concurrent lookups won't read a null pointer
      // This is a soft guarantee for cross thread invalidation, as atomics are not used
      // and it hasn't been thoroughly tested
    }

    if (!DisableL2Cache()) {
      // Do full map
      Address = Address & (VirtualMemSize - 1);
      uint64_t PageOffset = Address & (0x0FFF);
      Address >>= 12;

      uintptr_t* Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
      uint64_t LocalPagePointer = Pointers[Address];
      if (!LocalPagePointer) {
        // Page for this code didn't even exist, nothing to do
        return;
      }

      // Page exists, just set the offset to zero
      auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);
      BlockPointers[PageOffset].GuestCode = 0;
      BlockPointers[PageOffset].HostCode = 0;
    }
  }

  // Invalidates all L1/L2 entries for all guest block that intersect the given range
  bool InvalidateCacheRange(uint64_t Start, uint64_t Length) {
    auto lk = Shared->AcquireWriteLock();

    auto lower = CachedCodePages.lower_bound(Start >> 12);
    auto upper = CachedCodePages.upper_bound((Start + Length - 1) >> 12);

    for (auto it = lower; it != upper; it++) {
      for (const auto& Entry : it->second) {
        InvalidateCache(Entry, lk);
      }
    }
    bool ret = upper != lower;
    CachedCodePages.erase(lower, upper);
    return ret;
  }

  void AddBlockLink(uint64_t GuestDestination, FEXCore::Context::ExitFunctionLinkData* HostLink,
                    const FEXCore::Context::BlockDelinkerFunc& delinker, const LookupCacheWriteLockToken& lk) {
    Shared->AddBlockLink(GuestDestination, HostLink, delinker, lk);
  }

  void ClearCache(const LookupCacheWriteLockToken&);
  void ClearL2Cache(const LookupCacheBaseLockToken&);
  void ClearThreadLocalCaches(const LookupCacheWriteLockToken&);

  uintptr_t GetL1Pointer() const {
    return L1Pointer;
  }
  uintptr_t GetScaledL1PointerMask() const {
    return L1PointerMask << FEXCore::ilog2(sizeof(LookupCache::LookupCacheEntry));
  }
  uintptr_t GetPagePointer() const {
    return PagePointer;
  }
  uintptr_t GetVirtualMemorySize() const {
    return VirtualMemSize;
  }

  // This needs to be taken before reads or writes to L2, L3, CodePages,
  // and before writes to L1. Concurrent access from a thread that this LookupCache doesn't belong to
  // may only happen during cross thread invalidation (::Erase).
  // All other operations must be done from the owning thread.
  // Some care is taken so that L1 lookups can be done without locks, and even tearing is unlikely to lead to a crash.
  // This approach has not been fully vetted yet.
  // Also note that L1 lookups might be inlined in the JIT Dispatcher and/or block ends.
  auto AcquireWriteLock() {
    return Shared->AcquireWriteLock();
  }

private:
  void AddL1Entry(uint64_t GuestAddress, uint64_t HostCode) {
    auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[GuestAddress & L1PointerMask];
    L1Entry.GuestCode = GuestAddress;
    L1Entry.HostCode = HostCode;
  }

  void CacheBlockMapping(uint64_t Address, const GuestToHostMap::BlockEntry& Entry, bool L1Only, const LookupCacheBaseLockToken& lk) {
    for (const auto& CodePage : Entry.CodePages) {
      CachedCodePages[CodePage >> 12].insert(Address);
    }

    // Do L1
    AddL1Entry(Address, Entry.HostCode);

    if (!DisableL2Cache() && !L1Only) {
      // Do ful map
      auto FullAddress = Address;
      Address = Address & (VirtualMemSize - 1);

      uint64_t PageOffset = Address & (0x0FFF);
      Address >>= 12;

      uintptr_t* Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
      uint64_t LocalPagePointer = Pointers[Address];
      if (!LocalPagePointer) {
        // We don't have a page pointer for this address
        // Allocate one now if we can
        uintptr_t NewPageBacking = AllocateBackingForPage();
        if (!NewPageBacking) {
          // Couldn't allocate, clear L2 and retry
          ClearL2Cache(lk);
          CacheBlockMapping(FullAddress, Entry, false, lk);
          return;
        }
        Pointers[Address] = NewPageBacking;
        LocalPagePointer = NewPageBacking;
      }

      // Add the new pointer to the page block
      auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);

      // This silently replaces existing mappings
      BlockPointers[PageOffset].GuestCode = FullAddress;
      BlockPointers[PageOffset].HostCode = Entry.HostCode;
    }
  }

  uintptr_t AllocateBackingForPage() {
    uintptr_t NewBase = AllocateOffset;
    uintptr_t NewEnd = AllocateOffset + SIZE_PER_PAGE;

    if (NewEnd >= CurrentCodeSize) {
      // We ran out of block backing space. Need to clear the block cache and tell the JIT cores to clear their caches as well
      // Tell whatever is calling this that it needs to do it.
      return 0;
    }

    AllocateOffset = NewEnd;
    return PageMemory + NewBase;
  }

  // Maps from a page index to all blocks in the page that have at some point been fetched into L1/L2
  fextl::map<uint64_t, fextl::robin_set<uint64_t>> CachedCodePages;

  uintptr_t PagePointer;
  uintptr_t PageMemory;
  uintptr_t L1Pointer;
  uintptr_t L1PointerMask;

  size_t TotalCacheSize;

  // L1 is a fixed FIXED_L1_ENTRIES entries (see public section above) so that its base pointer and
  // index mask can be pinned/baked into JIT'd code. Dynamic resizing between these previously varied;
  // both bounds are now pinned to the same fixed size.
  constexpr static size_t MIN_L1_ENTRIES = FIXED_L1_ENTRIES;
  constexpr static size_t MAX_L1_ENTRIES = FIXED_L1_ENTRIES;

  // L2's page-block backing storage is dynamically resizable between these bounds (the heuristic
  // ported from L1's old dynamic sizing, see UpdateDynamicL2Stats). MAX_CODE_SIZE is reserved
  // upfront so growing never needs to relocate PageMemory or anything built on top of it (L1Pointer).
  constexpr static size_t MIN_L2_ENTRIES = 16 * 1024;        // Must be a power of 2
  constexpr static size_t MAX_L2_ENTRIES = 16 * 1024 * 1024; // Must be a power of 2
  constexpr static size_t MIN_CODE_SIZE = MIN_L2_ENTRIES * sizeof(LookupCacheEntry);
  constexpr static size_t MAX_CODE_SIZE = MAX_L2_ENTRIES * sizeof(LookupCacheEntry);

  constexpr static size_t SIZE_PER_PAGE = FEXCore::Utils::FEX_PAGE_SIZE * sizeof(LookupCacheEntry);
  constexpr static size_t MAX_L1_SIZE = MAX_L1_ENTRIES * sizeof(LookupCacheEntry);

  size_t AllocateOffset {};

  FEXCore::Context::ContextImpl* ctx;
  uint64_t VirtualMemSize {};

  size_t CurrentCodeSize = MIN_CODE_SIZE;
  uint64_t L2L3CacheHits {};
  std::chrono::time_point<std::chrono::system_clock> LastPeriod {};
  constexpr static std::chrono::seconds SamplePeriod {1};
  FEX_CONFIG_OPT(DynamicL1CacheIncreaseCountHeuristic, DYNAMICL1CACHEINCREASECOUNTHEURISTIC);
  FEX_CONFIG_OPT(DynamicL1CacheDecreaseCountHeuristic, DYNAMICL1CACHEDECREASECOUNTHEURISTIC);

  FEX_CONFIG_OPT(DynamicL1Cache, DYNAMICL1CACHE);
  FEX_CONFIG_OPT(DisableL2Cache, DISABLEL2CACHE);
};
} // namespace FEXCore
