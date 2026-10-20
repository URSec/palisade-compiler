//===- PalisadeLocalPromote.cpp - Protected locals ------------------------===//
//
//     Promoting provably safe locals into the protected address space.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Analyze allocas on ordinary address space, promote them to protected
// address space if we can identify their accesses always safe.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PalisadeLocalPromote.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

namespace {

constexpr unsigned ProtectedAddressSpace = 200;

// Classify the allocas that all its accesses are in range.
// Promote (re-tag) those allocas with protected address-space (AS 200).
class LocalClassifier {
  const DataLayout &DL;
  ScalarEvolution &SE;

  bool isAccessSafe(Value *Addr, uint64_t AccessSize, AllocaInst &AI,
                    uint64_t AllocaSize) {

    // A zero-length memory intrinsic cannot access the object.
    if (!AccessSize)
      return true;

    // Match the base pointer.
    const SCEV *AddrExpr = SE.getSCEV(Addr);
    const auto *Base = dyn_cast<SCEVUnknown>(SE.getPointerBase(AddrExpr));
    if (!Base || Base->getValue() != &AI)
      return false;

    // check overflow for that type.
    const SCEV *Offset = SE.removePointerBase(AddrExpr);
    unsigned Bits = SE.getTypeSizeInBits(Offset->getType());
    if (!AllocaSize || !isUIntN(Bits, AllocaSize) || !isUIntN(Bits, AccessSize))
      return false;

    // check access is in range.
    ConstantRange AccessRange = SE.getUnsignedRange(Offset).add(
        ConstantRange(APInt(Bits, 0), APInt(Bits, AccessSize)));
    ConstantRange AllocaRange(APInt(Bits, 0), APInt(Bits, AllocaSize));
    return AllocaRange.contains(AccessRange);
  }

public:
  LocalClassifier(const DataLayout &DL, ScalarEvolution &SE) : DL(DL), SE(SE) {}

  bool isProtectable(AllocaInst &AI, SetVector<Instruction *> &DerivedPtrs) {

    // only handle ordinary stack allocas.
    if (AI.getAddressSpace() != 0)
      return false;

    auto Size = AI.getAllocationSize(DL);
    if (!Size)
      return false;
    uint64_t AllocaSize = Size->getFixedValue();

    // Start a worklist to trace the def-use chain:
    // pushes every Instruction that is derived from the alloca pointer.
    DerivedPtrs.insert(&AI);
    for (unsigned Index = 0; Index != DerivedPtrs.size(); ++Index) {
      Instruction *Ptr = DerivedPtrs[Index];
      for (Use &U : Ptr->uses()) {
        auto *I = cast<Instruction>(U.getUser());

        switch (I->getOpcode()) {
        case Instruction::GetElementPtr:
          // A vector GEP yields <N x ptr>, which cannot be promoted in place.
          if (!I->getType()->isPointerTy())
            return false;
          [[fallthrough]];
        case Instruction::BitCast:
        case Instruction::PHI:
        case Instruction::Select:
        case Instruction::Freeze:
          DerivedPtrs.insert(I);
          break;
        case Instruction::Load:
          // Reads cannot corrupt the object or its neighbors.
          // We do not handle over read.
          break;
        case Instruction::Store: {
          // Storing the address escapes the object.
          if (U.getOperandNo() != StoreInst::getPointerOperandIndex())
            return false;

          // Check the store in range.
          uint64_t StoreSize =
              DL.getTypeStoreSize(I->getOperand(0)->getType()).getFixedValue();
          if (!isAccessSafe(Ptr, StoreSize, AI, AllocaSize))
            return false;
          break;
        }
        case Instruction::ICmp:
          // Comparing an address does not dereference or capture it.
          break;
        case Instruction::Call:
        case Instruction::Invoke: {
          auto &CB = cast<CallBase>(*I);

          // Check if its passed as an argument.
          if (!CB.isArgOperand(&U))
            return false;

          // Skip a lifetime marker.
          if (I->isLifetimeStartOrEnd())
            break;

          // Check the memory intrinsic is in bound.
          if (auto *MI = dyn_cast<MemIntrinsic>(I)) {
            // Only read?
            if (&U != &MI->getRawDestUse())
              break;

            // Check write in range.
            auto Len = MI->getLengthInBytes();
            if (!Len || Len->getActiveBits() > 64 ||
                !isAccessSafe(Ptr, Len->getZExtValue(), AI, AllocaSize))
              return false;
            break;
          }

          unsigned Arg = CB.getArgOperandNo(&U);
          // Check pass by value argument.
          // ABI copies can access the pointee even when the callee itself
          // accesses no memory.
          if (CB.isPassPointeeByValueArgument(Arg))
            return false;

          // Trust the external function call that not modify memory.
          if (!CB.doesNotCapture(Arg) ||
              !(CB.onlyReadsMemory(Arg) || CB.onlyReadsMemory()))
            return false;
          break;
        }
        default:
          return false;
        }
      }
    }

    // Check lineage on a family pointers.
    for (Instruction *Ptr : DerivedPtrs) {
      if (Ptr == &AI)
        continue;
      for (Value *Op : Ptr->operand_values()) {
        if (!Op->getType()->isPointerTy())
          continue;
        auto *OpI = dyn_cast<Instruction>(Op);
        if (!OpI || !DerivedPtrs.contains(OpI))
          return false;
      }
    }

    return true;
  }
};

// Tag the safe pointers with protected address space.
void promotePointers(const SetVector<Instruction *> &PromotedPtrs) {
  SetVector<IntrinsicInst *> Intrinsics;
  for (Instruction *Ptr : PromotedPtrs) {
    Type *OldTy = Ptr->getType();
    // Mutate pointer address-space in place.
    Ptr->mutateType(PointerType::get(Ptr->getContext(), ProtectedAddressSpace));

    // Fix type mismatch for its users.
    SmallVector<Use *> Uses;
    for (Use &U : Ptr->uses())
      Uses.push_back(&U);
    Value *GenericPtr = nullptr;

    for (Use *U : Uses) {
      auto *I = cast<Instruction>(U->getUser());
      if (PromotedPtrs.contains(I) || isa<LoadInst, StoreInst>(I))
        continue;

      // Handle intrinsic instructions like llvm.memcpy
      if (auto *II = dyn_cast<IntrinsicInst>(I)) {
        if (isa<MemIntrinsic>(II) || II->isLifetimeStartOrEnd()) {
          Intrinsics.insert(II);
          continue;
        }
      }

      // Add casts to preserve call signatures and pointer comparison
      // operand types.
      if (!GenericPtr) {
        auto InsertPt = isa<PHINode>(Ptr)
                            ? Ptr->getParent()->getFirstInsertionPt()
                            : std::next(Ptr->getIterator());
        auto *Cast = new AddrSpaceCastInst(
            Ptr, OldTy, Ptr->getName() + ".generic", InsertPt);
        Cast->setDebugLoc(Ptr->getDebugLoc());
        GenericPtr = Cast;
      }
      U->set(GenericPtr);
    }
  }

  // Remangle the intrinsic to preserve correct types and metadata.
  for (IntrinsicInst *II : Intrinsics) {
    SmallVector<Type *> ArgTypes;
    for (Value *Arg : II->args())
      ArgTypes.push_back(Arg->getType());
    II->setCalledFunction(Intrinsic::getOrInsertDeclaration(
        II->getModule(), II->getIntrinsicID(), II->getType(), ArgTypes));
  }
}

} // namespace

PreservedAnalyses PalisadeLocalPromotePass::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  const DataLayout &DL = F.getDataLayout();
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

  LocalClassifier Classifier(DL, SE);
  SetVector<Instruction *> PromotedPtrs;

  // Find all stack pointers that can be promoted.
  for (Instruction &I : instructions(F)) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI)
      continue;
    SetVector<Instruction *> DerivedPtrs;
    if (Classifier.isProtectable(*AI, DerivedPtrs))
      PromotedPtrs.insert(DerivedPtrs.begin(), DerivedPtrs.end());
  }
  if (PromotedPtrs.empty())
    return PreservedAnalyses::all();

  // Tag the poitners with protected address space.
  promotePointers(PromotedPtrs);
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
