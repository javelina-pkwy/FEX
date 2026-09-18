// SPDX-License-Identifier: MIT
/*
$info$
tags: ir|opts
desc: Removes redundant zeroing of the AVX upper halves held in the context (AVX-128 emulation)
$end_info$
*/

#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/fextl/deque.h>
#include <FEXCore/fextl/vector.h>

#include <bit>
#include <cstdint>

namespace FEXCore::IR {

namespace {
// Without 256-bit vectors on the host, the upper 128 bits of every YMM register live in
// CPUState::avx_high. Every VEX.128 instruction zeroes the upper half of its destination, and the
// register cache flushes those zeroes to the context at the end of every basic block, so code that
// stays in VEX.128 land rewrites the same zeroes on every block. Track which slots are known to hold
// zero at each block entry and drop the redundant stores (and loads).
constexpr uint32_t HighBase = offsetof(FEXCore::Core::CPUState, avx_high);
constexpr uint32_t HighSlotSize = sizeof(FEXCore::Core::CPUState::avx_high[0]);
constexpr uint32_t HighSlots = sizeof(FEXCore::Core::CPUState::avx_high) / HighSlotSize;
constexpr uint32_t HighEnd = HighBase + HighSlots * HighSlotSize;
static_assert(HighSlots <= 32);

using SlotMask = uint32_t;
constexpr SlotMask AllSlots = (1u << HighSlots) - 1;

bool Overlaps(uint32_t Offset, uint32_t Size) {
  return Offset < HighEnd && Offset + Size > HighBase;
}

// Slots any byte of [Offset, Offset + Size) lands in.
SlotMask TouchedSlots(uint32_t Offset, uint32_t Size) {
  if (!Overlaps(Offset, Size)) {
    return 0;
  }
  const uint32_t First = std::max(Offset, HighBase);
  const uint32_t Last = std::min(Offset + Size, HighEnd) - 1;
  SlotMask Mask = 0;
  for (uint32_t Slot = (First - HighBase) / HighSlotSize; Slot <= (Last - HighBase) / HighSlotSize; ++Slot) {
    Mask |= 1u << Slot;
  }
  return Mask;
}

// Slots entirely inside [Offset, Offset + Size).
SlotMask CoveredSlots(uint32_t Offset, uint32_t Size) {
  if (!Overlaps(Offset, Size)) {
    return 0;
  }
  const uint32_t First = std::max(Offset, HighBase);
  const uint32_t End = std::min(Offset + Size, HighEnd);
  SlotMask Mask = 0;
  for (uint32_t Slot = 0; Slot < HighSlots; ++Slot) {
    const uint32_t SlotBase = HighBase + Slot * HighSlotSize;
    if (SlotBase >= First && SlotBase + HighSlotSize <= End) {
      Mask |= 1u << Slot;
    }
  }
  return Mask;
}

struct Effect {
  SlotMask Zero {};    // slots that hold zero after the op
  SlotMask NonZero {}; // slots whose content is no longer known to be zero
  bool KillAll {};
};

struct BlockState {
  Ref Node {};
  SlotMask Gen {};
  SlotMask Kill {};
  SlotMask In {};
  SlotMask Out {AllSlots};
  bool Entry {};
  bool InWorklist {};
  fextl::vector<uint32_t> Preds;
  fextl::vector<uint32_t> Succs;
};

class AVXHighZeroElimination final : public Pass {
public:
  void Run(IREmitter* IREmit) override;

private:
  bool IsZeroValue(IRListView& IR, OrderedNodeWrapper Value) const;
  Effect Classify(IRListView& IR, IROp_Header* IROp) const;
};

bool AVXHighZeroElimination::IsZeroValue(IRListView& IR, OrderedNodeWrapper Value) const {
  auto Op = IR.GetOp<IROp_Header>(Value);
  switch (Op->Op) {
  case OP_LOADNAMEDVECTORCONSTANT: return Op->C<IROp_LoadNamedVectorConstant>()->Constant == NamedVectorConstant::NAMED_VECTOR_ZERO;
  case OP_INLINECONSTANT: return Op->C<IROp_InlineConstant>()->Constant == 0;
  case OP_CONSTANT: return Op->C<IROp_Constant>()->Constant == 0;
  default: return false;
  }
}

Effect AVXHighZeroElimination::Classify(IRListView& IR, IROp_Header* IROp) const {
  Effect E;
  switch (IROp->Op) {
  case OP_STORECONTEXT: {
    auto Op = IROp->C<IROp_StoreContext>();
    const uint32_t Size = OpSizeToSize(IROp->Size);
    const SlotMask Touched = TouchedSlots(Op->Offset, Size);
    if (Touched && Size == HighSlotSize && CoveredSlots(Op->Offset, Size) == Touched && IsZeroValue(IR, Op->Value)) {
      E.Zero = Touched;
    } else {
      E.NonZero = Touched;
    }
    break;
  }
  case OP_STORECONTEXTPAIR: {
    auto Op = IROp->C<IROp_StoreContextPair>();
    const uint32_t Size = OpSizeToSize(IROp->Size);
    const SlotMask Touched = TouchedSlots(Op->Offset, Size * 2);
    if (!Touched) {
      break;
    }
    if (Size == HighSlotSize && CoveredSlots(Op->Offset, Size * 2) == Touched) {
      // Two whole slots, one per value.
      const SlotMask First = TouchedSlots(Op->Offset, Size);
      const SlotMask Second = TouchedSlots(Op->Offset + Size, Size);
      (IsZeroValue(IR, Op->Value1) ? E.Zero : E.NonZero) |= First;
      (IsZeroValue(IR, Op->Value2) ? E.Zero : E.NonZero) |= Second;
    } else if (Size * 2 == HighSlotSize && CoveredSlots(Op->Offset, Size * 2) == Touched) {
      // Two GPR-sized halves making up one slot.
      (IsZeroValue(IR, Op->Value1) && IsZeroValue(IR, Op->Value2) ? E.Zero : E.NonZero) |= Touched;
    } else {
      E.NonZero = Touched;
    }
    break;
  }
  case OP_CONTEXTCLEAR: {
    auto Op = IROp->C<IROp_ContextClear>();
    const SlotMask Covered = CoveredSlots(Op->Offset, Op->Size);
    E.Zero = Covered;
    E.NonZero = TouchedSlots(Op->Offset, Op->Size) & ~Covered;
    break;
  }
  case OP_STORECONTEXTINDEXED:
  case OP_SYSCALL:
  case OP_THUNK:
  case OP_BREAK:
  case OP_CALLBACKRETURN: E.KillAll = true; break;
  default: break;
  }
  return E;
}

void AVXHighZeroElimination::Run(IREmitter* IREmit) {
  auto CurrentIR = IREmit->ViewIR();
  auto Header = CurrentIR.GetHeader();
  if (!Header->UsesAVXHigh || Header->BlockCount < 2) {
    return;
  }
  FEXCORE_PROFILE_SCOPED("PassManager::AVXHighZero");

  fextl::vector<BlockState> Blocks(Header->BlockCount);

  // Gather the CFG and each block's transfer function.
  for (auto [BlockNode, BlockHeader] : CurrentIR.GetBlocks()) {
    auto Block = BlockHeader->C<IROp_CodeBlock>();
    auto& State = Blocks[Block->ID];
    State.Node = BlockNode;
    State.Entry = Block->EntryPoint;

    for (auto [CodeNode, IROp] : CurrentIR.GetCode(BlockNode)) {
      const Effect E = Classify(CurrentIR, IROp);
      if (E.KillAll) {
        State.Kill = AllSlots;
        State.Gen = 0;
        continue;
      }
      State.Gen = (State.Gen & ~E.NonZero) | E.Zero;
      State.Kill = (State.Kill & ~E.Zero) | E.NonZero;
    }

    auto CodeLast = CurrentIR.at(Block->Last);
    --CodeLast;
    auto [ExitNode, ExitOp] = CodeLast();
    auto AddEdge = [&](OrderedNodeWrapper To) {
      const uint32_t ToID = CurrentIR.GetOp<IROp_CodeBlock>(To)->ID;
      State.Succs.push_back(ToID);
      Blocks[ToID].Preds.push_back(Block->ID);
    };
    if (ExitOp->Op == OP_CONDJUMP) {
      auto Op = ExitOp->C<IROp_CondJump>();
      AddEdge(Op->TrueBlock);
      AddEdge(Op->FalseBlock);
    } else if (ExitOp->Op == OP_JUMP) {
      AddEdge(ExitOp->C<IROp_Jump>()->TargetBlock);
    }
  }

  // Forward dataflow to a fixed point. A slot is known zero at block entry only if it is known zero
  // at the exit of every predecessor; entry points can be reached from the dispatcher with any state.
  fextl::deque<uint32_t> Worklist;
  for (uint32_t ID = 0; ID < Blocks.size(); ++ID) {
    Worklist.push_back(ID);
    Blocks[ID].InWorklist = true;
  }
  auto ComputeIn = [&](BlockState& State) -> SlotMask {
    if (State.Entry || State.Preds.empty()) {
      return 0;
    }
    SlotMask In = AllSlots;
    for (uint32_t Pred : State.Preds) {
      In &= Blocks[Pred].Out;
    }
    return In;
  };
  while (!Worklist.empty()) {
    auto& State = Blocks[Worklist.front()];
    Worklist.pop_front();
    State.InWorklist = false;

    State.In = ComputeIn(State);
    const SlotMask Out = (State.In & ~State.Kill) | State.Gen;
    if (Out != State.Out) {
      State.Out = Out;
      for (uint32_t Succ : State.Succs) {
        if (!Blocks[Succ].InWorklist) {
          Blocks[Succ].InWorklist = true;
          Worklist.push_back(Succ);
        }
      }
    }
  }

  // Rewrite: drop stores of zero into slots already zero, and read known-zero slots as a constant.
  fextl::vector<Ref> Removals;
  fextl::vector<Ref> ZeroLoads;
  for (auto& State : Blocks) {
    if (!State.Node) {
      continue;
    }
    SlotMask Zero = ComputeIn(State);
    Removals.clear();
    ZeroLoads.clear();

    for (auto [CodeNode, IROp] : CurrentIR.GetCode(State.Node)) {
      if (IROp->Op == OP_LOADCONTEXT) {
        auto Op = IROp->C<IROp_LoadContext>();
        const uint32_t Size = OpSizeToSize(IROp->Size);
        const SlotMask Touched = TouchedSlots(Op->Offset, Size);
        if (Touched && Size == HighSlotSize && CoveredSlots(Op->Offset, Size) == Touched && (Zero & Touched) == Touched) {
          ZeroLoads.push_back(CodeNode);
        }
        continue;
      }

      const Effect E = Classify(CurrentIR, IROp);
      if (E.KillAll) {
        Zero = 0;
        continue;
      }
      if (E.Zero && !E.NonZero && (Zero & E.Zero) == E.Zero) {
        // Everything this op writes is already zero in memory.
        Removals.push_back(CodeNode);
        continue;
      }
      Zero = (Zero & ~E.NonZero) | E.Zero;
    }

    for (Ref Node : Removals) {
      IREmit->Remove(Node);
    }
    for (Ref Node : ZeroLoads) {
      IREmit->SetWriteCursorBefore(Node);
      Ref ZeroVector = IREmit->_LoadNamedVectorConstant(OpSize::i128Bit, NamedVectorConstant::NAMED_VECTOR_ZERO);
      IREmit->ReplaceUsesWithAfter(Node, ZeroVector, Node);
      IREmit->Remove(Node);
    }
  }
}
} // namespace

fextl::unique_ptr<Pass> CreateAVXHighZeroElimination() {
  return fextl::make_unique<AVXHighZeroElimination>();
}

} // namespace FEXCore::IR
