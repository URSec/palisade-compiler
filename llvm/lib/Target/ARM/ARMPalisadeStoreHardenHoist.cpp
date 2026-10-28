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
STATISTIC(NumStores, "Number of FP stores kept native behind a range check");

namespace {

// TTT result register (ARMv8-M): bit 16 = MRVALID (MPU region
// valid), bit 19 = RW (unprivileged read/write permitted). Both set means an
// unprivileged store to the address is allowed.
constexpr uint32_t TTMask = (1u << 16) | (1u << 19);

// One floating-point store proven to stay inside a single invariant range.
struct GuardedStore {
  StoreInst *Store;
  const SCEV *Lo;
  const SCEV *End;
  uint64_t Size;
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

  bool collectLoop(Loop *L, SmallVectorImpl<GuardedStore> &Out);
  void applyGuard(Loop *L, ArrayRef<GuardedStore> Stores);
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

// Gather the FP stores of L whose address is an affine recurrence of L with a
// known-positive step, recording each one's written byte range.
bool ARMPalisadeStoreHardenHoist::collectLoop(
    Loop *L, SmallVectorImpl<GuardedStore> &Out) {

  if (!L->getLoopPreheader() || !L->getLoopLatch())
    return false;

  if (!isLoopCallSafe(L))
    return false;

  // Must be SCEV computable for trip count.
  const SCEV *BTC = SE->getBackedgeTakenCount(L);
  if (isa<SCEVCouldNotCompute>(BTC))
    return false;

  const DataLayout &DL = L->getHeader()->getModule()->getDataLayout();
  SCEVExpander Check(*SE, "palisade.check");

  const Instruction *IP = L->getLoopPreheader()->getTerminator();

  for (BasicBlock *BB : L->blocks()) {
    // Only handle the innder most loop.
    if (LI->getLoopFor(BB) != L)
      continue;
    for (Instruction &I : *BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || !SI->isSimple())
        continue;

      // Only for FP
      Type *Ty = SI->getValueOperand()->getType();
      if (!Ty->isFloatTy() && !Ty->isDoubleTy())
        continue;

      // addrspace 200 is already privileged.
      if (SI->getPointerAddressSpace() != 0)
        continue;

      // Index must be monotone.
      const SCEV *Ptr = SE->getSCEV(SI->getPointerOperand());
      const auto *AR = dyn_cast<SCEVAddRecExpr>(Ptr);
      if (!AR || AR->getLoop() != L || !AR->isAffine())
        continue;

      // Get recurrence
      const SCEV *Step = AR->getStepRecurrence(*SE);
      if (!SE->isLoopInvariant(Step, L) || !SE->isKnownPositive(Step))
        continue;

      // lo = first element's address,
      // end = last element's address.
      const SCEV *Lo = AR->getStart();
      const SCEV *End = AR->evaluateAtIteration(BTC, *SE);
      uint64_t Size = DL.getTypeStoreSize(Ty);
      if (!Check.isSafeToExpandAt(Lo, IP) || !Check.isSafeToExpandAt(End, IP))
        continue;

      Out.push_back({SI, Lo, End, Size});
    }
  }
  return !Out.empty();
}

void ARMPalisadeStoreHardenHoist::applyGuard(Loop *L,
                                             ArrayRef<GuardedStore> Stores) {
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
  for (const GuardedStore &GS : Stores) {
    Value *LoP = Expander.expandCodeFor(GS.Lo, I8Ptr, IP);
    Value *EndP = Expander.expandCodeFor(GS.End, I8Ptr, IP);

    // Last written byte: End + (Size - 1).
    Value *HiP = GS.Size > 1 ? B.CreateInBoundsGEP(B.getInt8Ty(), EndP,
                                                   B.getInt32(GS.Size - 1))
                             : EndP;

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
    ++NumStores;
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
  for (const GuardedStore &GS : Stores)
    GS.Store->setMetadata("palisade.noharden", Tag);

  ++NumGuarded;
}

bool ARMPalisadeStoreHardenHoist::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();

  // Find the stores in the loop that its check can be hoisted.
  SmallVector<std::pair<Loop *, SmallVector<GuardedStore, 4>>, 8> Work;
  for (Loop *L : LI->getLoopsInPreorder()) {
    SmallVector<GuardedStore, 4> Stores;
    if (collectLoop(L, Stores))
      Work.emplace_back(L, std::move(Stores));
  }

  // Hoist the checks outside the loop
  for (auto &[L, Stores] : Work)
    applyGuard(L, Stores);

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
