//===- ARMPalisadeStoreHardenHoist.cpp - Hoist store checks out of loops --===//
//
//     Keeping privilege stores in loops by hoist checks using TTT.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Store hardening rewrites every ordinary store into an unprivileged store
// that can affect the performance. Specifically, for a floating-point store
// there is no unprivileged version, so the rewrite becomes VMOV + STRT,
// whose VMOV->STRT RAW dependency stalls the in-order pipeline.
//
// This pass uses TTT instruction to replace the store hardening when FP
// computation is in the inner most loop. For a call-free canoical loop,
// whose FP store address has a monotone [lo, hi] range, we can hoist the
// check out of the loop by using TTT instructions.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsARM.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

using namespace llvm;

#define DEBUG_TYPE "arm-palisade-store-harden-hoist"

STATISTIC(NumGuarded, "Number of loops given a hoisted store-range check");
STATISTIC(NumChecks, "Number of TTT range checks emitted");
STATISTIC(NumStores, "Number of FP stores kept native behind a range check");

namespace {

// TTT result register (ARMv8-M): bit 16 = MRVALID (MPU region
// valid), bit 19 = RW (unprivileged read/write permitted). Both set means an
// unprivileged store to the address is allowed.
constexpr uint32_t TTMask = (1u << 16) | (1u << 19);

// One floating-point store and the bytes [Lo, Hi] it may write while the loop.
struct GuardedStore {
  StoreInst *Store;
  const SCEV *Lo;
  const SCEV *Hi;
};

// Stores into one object, grouped together by one range [Lo, Hi].
struct GuardedRange {
  const SCEV *Lo;
  const SCEV *Hi;
  SmallVector<StoreInst *, 8> Stores;
};

class ARMPalisadeStoreHardenHoist : public FunctionPass {
public:
  static char ID;
  ARMPalisadeStoreHardenHoist() : FunctionPass(ID) {
    initializeARMPalisadeStoreHardenHoistPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override {
    return "ARM Palisade store harden hoist";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
  }

  bool runOnFunction(Function &F) override;

private:
  ScalarEvolution *SE = nullptr;
  LoopInfo *LI = nullptr;
  DominatorTree *DT = nullptr;
  DenseMap<const Loop *, bool> Checkable;

  bool canCheckBefore(const Loop *L);
  bool canExpandBefore(const GuardedStore &GS, const Loop *L);
  const SCEVAddRecExpr *increasingRecurrence(const SCEV *S, const Loop *L);
  const SCEV *lowestInLoop(const SCEV *S, const Loop *L);
  const SCEV *highestInLoop(const SCEV *S, const Loop *L);
  std::optional<GuardedStore> rangeInLoop(StoreInst *SI, Loop *L);
  Loop *hoistCheck(GuardedStore &GS, Loop *L);
  void mergeIntoRanges(SmallVectorImpl<GuardedRange> &Ranges,
                       const GuardedStore &GS);
  void applyGuard(Loop *L, ArrayRef<GuardedRange> Ranges);
};

} // end anonymous namespace

// Check is safe to be hoisted if the loop contains no
// function calls and no intrinsic calls with observable side effects.
static bool isLoopCallSafe(const Loop *L) {
  for (const BasicBlock *BB : L->blocks())
    for (const Instruction &I : *BB)
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!isa<IntrinsicInst>(CB) || CB->mayHaveSideEffects())
          return false;
  return true;
}

// A check can be hoisted before L if L has a preheader and a latch and is
// call-free.
bool ARMPalisadeStoreHardenHoist::canCheckBefore(const Loop *L) {
  auto [It, Inserted] = Checkable.try_emplace(L, false);
  if (Inserted)
    It->second =
        L->getLoopPreheader() && L->getLoopLatch() && isLoopCallSafe(L);
  return It->second;
}

// Whether both bounds of lo,hi can be computed before the loop.
bool ARMPalisadeStoreHardenHoist::canExpandBefore(const GuardedStore &GS,
                                                  const Loop *L) {
  SCEVExpander Check(*SE, "palisade.check");
  const Instruction *IP = L->getLoopPreheader()->getTerminator();
  return Check.isSafeToExpandAt(GS.Lo, IP) && Check.isSafeToExpandAt(GS.Hi, IP);
}

// S as a recurrence of {Start,+,Step}<L> with known-positive Step.
const SCEVAddRecExpr *
ARMPalisadeStoreHardenHoist::increasingRecurrence(const SCEV *S,
                                                  const Loop *L) {
  const auto *AR = dyn_cast<SCEVAddRecExpr>(S);
  if (!AR || AR->getLoop() != L || !AR->isAffine())
    return nullptr;

  const SCEV *Step = AR->getStepRecurrence(*SE);
  if (!SE->isLoopInvariant(Step, L) || !SE->isKnownPositive(Step))
    return nullptr;
  return AR;
}

// The lowest value S takes when L runs, or null if unknown.
const SCEV *ARMPalisadeStoreHardenHoist::lowestInLoop(const SCEV *S,
                                                      const Loop *L) {
  if (SE->isLoopInvariant(S, L))
    return S;
  const SCEVAddRecExpr *AR = increasingRecurrence(S, L);
  return AR ? AR->getStart() : nullptr;
}

// The highest value S takes when L runs, or null if unknown.
const SCEV *ARMPalisadeStoreHardenHoist::highestInLoop(const SCEV *S,
                                                       const Loop *L) {
  if (SE->isLoopInvariant(S, L))
    return S;
  const SCEVAddRecExpr *AR = increasingRecurrence(S, L);
  if (!AR)
    return nullptr;

  // Must be SCEV computable for trip count.
  const SCEV *BTC = SE->getBackedgeTakenCount(L);
  if (isa<SCEVCouldNotCompute>(BTC))
    return nullptr;
  return AR->evaluateAtIteration(BTC, *SE);
}

// The bytes range that stores may write when L runs.
std::optional<GuardedStore>
ARMPalisadeStoreHardenHoist::rangeInLoop(StoreInst *SI, Loop *L) {
  if (!SI->isSimple())
    return std::nullopt;

  // Only for FP
  Type *Ty = SI->getValueOperand()->getType();
  if (!Ty->isFloatTy() && !Ty->isDoubleTy())
    return std::nullopt;

  // addrspace 200 is already privileged.
  if (SI->getPointerAddressSpace() != 0)
    return std::nullopt;

  // Index must be monotone.
  const SCEV *Ptr = SE->getSCEV(SI->getPointerOperand());
  const auto *AR = dyn_cast<SCEVAddRecExpr>(Ptr);
  if (!AR || AR->getLoop() != L)
    return std::nullopt;

  // lo = first element's address, end = last element's address.
  const SCEV *Lo = lowestInLoop(Ptr, L);
  const SCEV *End = highestInLoop(Ptr, L);
  if (!Lo || !End)
    return std::nullopt;

  // Last written byte: End + (Size - 1).
  uint64_t Size = SI->getDataLayout().getTypeStoreSize(Ty);
  Type *IntTy = SE->getEffectiveSCEVType(End->getType());
  const SCEV *Hi = SE->getAddExpr(End, SE->getConstant(IntTy, Size - 1));

  GuardedStore GS = {SI, Lo, Hi};
  if (!canExpandBefore(GS, L))
    return std::nullopt;
  return GS;
}

// Move checks from before L out through the enclosing loops it can be
// placed before.
Loop *ARMPalisadeStoreHardenHoist::hoistCheck(GuardedStore &GS, Loop *L) {
  while (Loop *Outer = L->getParentLoop()) {
    if (!canCheckBefore(Outer))
      break;

    const SCEV *Lo = lowestInLoop(GS.Lo, Outer);
    const SCEV *Hi = highestInLoop(GS.Hi, Outer);
    if (!Lo || !Hi)
      break;

    GuardedStore Wide = {GS.Store, Lo, Hi};
    if (!canExpandBefore(Wide, Outer))
      break;

    GS = Wide;
    L = Outer;
  }
  return L;
}

// Merge checks for the same memory objects in the loop.
void ARMPalisadeStoreHardenHoist::mergeIntoRanges(
    SmallVectorImpl<GuardedRange> &Ranges, const GuardedStore &GS) {
  for (GuardedRange &R : Ranges) {
    std::optional<APInt> LoDiff = SE->computeConstantDifference(GS.Lo, R.Lo);
    std::optional<APInt> HiDiff = SE->computeConstantDifference(GS.Hi, R.Hi);
    if (!LoDiff || !HiDiff)
      continue;

    if (LoDiff->isNegative())
      R.Lo = GS.Lo;
    if (HiDiff->isStrictlyPositive())
      R.Hi = GS.Hi;
    R.Stores.push_back(GS.Store);
    return;
  }
  Ranges.push_back({GS.Lo, GS.Hi, {GS.Store}});
}

void ARMPalisadeStoreHardenHoist::applyGuard(Loop *L,
                                             ArrayRef<GuardedRange> Ranges) {
  BasicBlock *PH = L->getLoopPreheader();
  Function *F = PH->getParent();
  LLVMContext &Ctx = F->getContext();

  // Split a new preheader (NB) off the bottom.
  BasicBlock *NB = SplitBlock(PH, PH->getTerminator(), DT, LI);

  // the new unconditional branch to NB
  Instruction *IP = PH->getTerminator();
  IRBuilder<> B(IP);
  SCEVExpander Expander(*SE, "palisade.range");
  Type *I8Ptr = PointerType::get(Ctx, 0);

  // Create check instructions (TTT)
  Value *AllOK = nullptr;
  for (const GuardedRange &R : Ranges) {
    Value *LoP = Expander.expandCodeFor(R.Lo, I8Ptr, IP);
    Value *HiP = Expander.expandCodeFor(R.Hi, I8Ptr, IP);

    // Test target on Low
    Value *TLo =
        B.CreateIntrinsic(B.getInt32Ty(), Intrinsic::arm_cmse_ttt, {LoP});

    // Test target on high
    Value *THi =
        B.CreateIntrinsic(B.getInt32Ty(), Intrinsic::arm_cmse_ttt, {HiP});

    // Compare the High and Low should be the same.
    Value *SameRegion = B.CreateICmpEQ(TLo, THi);

    // Mask the permission bit and check permission.
    Value *Writable = B.CreateICmpEQ(B.CreateAnd(TLo, TTMask),
                                     ConstantInt::get(B.getInt32Ty(), TTMask));
    Value *Cond = B.CreateAnd(SameRegion, Writable);
    AllOK = AllOK ? B.CreateAnd(AllOK, Cond) : Cond;
    ++NumChecks;
  }

  // On failure trap.
  BasicBlock *Trap =
      BasicBlock::Create(Ctx, PH->getName() + ".palisade.fail", F);
  IRBuilder<> TB(Trap);
  TB.CreateIntrinsic(TB.getVoidTy(), Intrinsic::trap, {});
  TB.CreateUnreachable();
  DT->addNewBlock(Trap, PH);

  // Replace PH's unconditional edge to the preheader with the test.
  B.SetInsertPoint(IP);
  B.CreateCondBr(AllOK, NB, Trap);
  IP->eraseFromParent();

  // Tag the proven stores so store hardening keeps them unchanged.
  MDNode *Tag = MDNode::get(Ctx, {});
  for (const GuardedRange &R : Ranges)
    for (StoreInst *SI : R.Stores) {
      SI->setMetadata("palisade.noharden", Tag);
      ++NumStores;
    }

  ++NumGuarded;
}

bool ARMPalisadeStoreHardenHoist::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();

  Checkable.clear();

  // For each FP store whose check can be hoisted, find the outermost loop its
  // check can be placed before, and merge it there with the stores into the
  // same object.
  MapVector<Loop *, SmallVector<GuardedRange, 4>> Work;
  for (Loop *L : LI->getLoopsInPreorder()) {
    if (!canCheckBefore(L))
      continue;
    for (BasicBlock *BB : L->blocks()) {
      // Only handle the innder most loop.
      if (LI->getLoopFor(BB) != L)
        continue;
      for (Instruction &I : *BB) {
        auto *SI = dyn_cast<StoreInst>(&I);
        if (!SI)
          continue;
        std::optional<GuardedStore> GS = rangeInLoop(SI, L);
        if (!GS)
          continue;
        Loop *Target = hoistCheck(*GS, L);
        mergeIntoRanges(Work[Target], *GS);
      }
    }
  }

  // Hoist the checks outside the loop
  for (auto &[L, Ranges] : Work)
    applyGuard(L, Ranges);

  return !Work.empty();
}

char ARMPalisadeStoreHardenHoist::ID = 0;

INITIALIZE_PASS_BEGIN(ARMPalisadeStoreHardenHoist, DEBUG_TYPE,
                      "ARM Palisade store harden hoist", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(ARMPalisadeStoreHardenHoist, DEBUG_TYPE,
                    "ARM Palisade store harden hoist", false, false)

FunctionPass *llvm::createARMPalisadeStoreHardenHoistPass() {
  return new ARMPalisadeStoreHardenHoist();
}
