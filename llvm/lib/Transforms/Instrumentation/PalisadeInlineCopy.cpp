//===- PalisadeInlineCopy.cpp - Inline protected copies -------------------===//
//
//     Keep compiler-generated copies into protected storage
//     out of the C library.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Expand llvm.memcpy/llvm.memmove/llvm.memset into inlined store instructions
// instead of calling C library.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PalisadeInlineCopy.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LowerMemIntrinsics.h"

using namespace llvm;

namespace {

constexpr unsigned ProtectedAddressSpace = 200;

void alignToWord(MemIntrinsic *MI, const DataLayout &DL) {
  Align Dest = getOrEnforceKnownAlignment(MI->getRawDest(), Align(4), DL, MI);
  MI->setDestAlignment(std::max(Dest, MI->getDestAlign().valueOrOne()));
  if (auto *Transfer = dyn_cast<MemTransferInst>(MI)) {
    Align Source =
        getOrEnforceKnownAlignment(Transfer->getRawSource(), Align(4), DL, MI);
    Transfer->setSourceAlignment(
        std::max(Source, Transfer->getSourceAlign().valueOrOne()));
  }
}

// Switch a llvm.memcpy or llvm.memset to llvm.memcpy.inline or
// llvm.memset.inline
void useInlineIntrinsic(MemIntrinsic *MI) {
  SmallVector<Type *, 3> OverloadTypes{MI->getRawDest()->getType()};
  Intrinsic::ID InlineID = Intrinsic::memset_inline;
  if (auto *Memcpy = dyn_cast<MemCpyInst>(MI)) {
    OverloadTypes.push_back(Memcpy->getRawSource()->getType());
    InlineID = Intrinsic::memcpy_inline;
  }
  OverloadTypes.push_back(MI->getLength()->getType());
  MI->setCalledFunction(Intrinsic::getOrInsertDeclaration(
      MI->getModule(), InlineID, OverloadTypes));
}

// Replace the copy with a loop of stores. A memmove LLVM cannot expand, one
// from ordinary memory, stays a library call.
void replaceWithLoop(MemIntrinsic *MI, const TargetTransformInfo &TTI) {
  if (auto *Memcpy = dyn_cast<MemCpyInst>(MI))
    expandMemCpyAsLoop(Memcpy, TTI);
  else if (auto *Memset = dyn_cast<MemSetInst>(MI))
    expandMemSetAsLoop(Memset, TTI);
  else if (!expandMemMoveAsLoop(cast<MemMoveInst>(MI), TTI))
    return;
  MI->eraseFromParent();
}

} // namespace

PreservedAnalyses PalisadeInlineCopyPass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
  SmallVector<MemIntrinsic *> Copies;
  // Find all the copy intrinsics.
  for (Instruction &I : instructions(F))
    if (auto *MI = dyn_cast<MemIntrinsic>(&I))
      if (MI->getDestAddressSpace() == ProtectedAddressSpace &&
          !MI->isForceInlined())
        Copies.push_back(MI);
  if (Copies.empty())
    return PreservedAnalyses::all();

  const TargetTransformInfo &TTI = AM.getResult<TargetIRAnalysis>(F);
  uint64_t InlineLimit = TTI.getMaxMemIntrinsicInlineSizeThreshold();
  for (MemIntrinsic *MI : Copies) {
    alignToWord(MI, F.getDataLayout());
    // constant size llvm.memcpy and llvm.memset uses inline intrinsics.
    // variable sized, or constant size that exceed limit transform
    // operations into store loop.
    auto *Size = dyn_cast<ConstantInt>(MI->getLength());
    if (Size && Size->getZExtValue() <= InlineLimit && !isa<MemMoveInst>(MI))
      useInlineIntrinsic(MI);
    else
      replaceWithLoop(MI, TTI);
  }
  return PreservedAnalyses::none();
}
