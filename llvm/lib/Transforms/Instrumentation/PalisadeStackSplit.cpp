//===- PalisadeStackSplit.cpp - Separate ordinary stack -------------------===//
//
//     Moving ordinary locals off the protected hardware stack.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Identify the locals with ordinary address space, and move them to
// a separated stack.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PalisadeStackSplit.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

constexpr unsigned ProtectedAddressSpace = 200;
constexpr Align StackAlignment = Align::Constant<8>();
constexpr StringLiteral UnsafeStackPtrName = "__palisade_unsafe_stack_ptr";
constexpr StringLiteral StackTopName = "__palisade_unsafe_stack_top";

class StackSplitter {
  Function &F;
  const DataLayout &DL;
  PointerType *PtrTy;
  IntegerType *IntPtrTy;
  GlobalVariable *UnsafeStackPtr = nullptr;

  // Static objects, each with its offset below the frame base.
  SmallVector<std::pair<Value *, uint64_t>> Objects;
  SmallVector<AllocaInst *> DynamicAllocas;
  SmallVector<IntrinsicInst *> StackSaves, StackRestores;
  SmallVector<ReturnInst *> Returns;
  DenseMap<Value *, Value *> SavedStackPtrs;
  uint64_t FrameSize = 0;
  Align FrameAlign = StackAlignment;

  // Volatile keeps every unsafe stack pointer update in program order.
  Value *loadUnsafeStackPtr(IRBuilder<> &B, const Twine &Name) {
    return B.CreateLoad(PtrTy, UnsafeStackPtr, /*isVolatile=*/true, Name);
  }

  void storeUnsafeStackPtr(IRBuilder<> &B, Value *Ptr) {
    B.CreateStore(Ptr, UnsafeStackPtr, /*isVolatile=*/true);
  }

  // Round down address using ptrmask intrinsic.
  Value *alignDown(IRBuilder<> &B, Value *Ptr, Align Alignment) {
    return B.CreateIntrinsic(
        PtrTy, Intrinsic::ptrmask,
        {Ptr, ConstantInt::getSigned(IntPtrTy, -int64_t(Alignment.value()))});
  }

  // Objects grow down from the frame base; an empty one still gets a byte.
  void addObject(Value *Object, uint64_t Size, Align Alignment) {
    FrameSize = alignTo(FrameSize + std::max<uint64_t>(Size, 1), Alignment);
    Objects.push_back({Object, FrameSize});
    FrameAlign = std::max(FrameAlign, Alignment);
  }

  void replaceAlloca(AllocaInst *AI, Value *Address) {
    // Lifetime markers only take allocas, the object now lives until return.
    for (User *U : make_early_inc_range(AI->users()))
      if (auto *II = dyn_cast<IntrinsicInst>(U);
          II && II->isLifetimeStartOrEnd())
        II->eraseFromParent();

    AI->replaceAllUsesWith(Address);
    AI->eraseFromParent();
  }

  // Find the unsafe stack pointer saved along with a stacksave token.
  Value *getSavedStackPtr(Value *Token) {
    if (Value *Saved = SavedStackPtrs.lookup(Token))
      return Saved;
    if (auto *PN = dyn_cast<PHINode>(Token)) {
      auto *Saved = PHINode::Create(PtrTy, PN->getNumIncomingValues(),
                                    "palisade.saved", PN->getIterator());
      // Map the PHI first, so a loop-carried token ends the recursion.
      SavedStackPtrs[PN] = Saved;
      for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I)
        Saved->addIncoming(getSavedStackPtr(PN->getIncomingValue(I)),
                           PN->getIncomingBlock(I));
      return Saved;
    }
    if (auto *SI = dyn_cast<SelectInst>(Token)) {
      Value *TrueSaved = getSavedStackPtr(SI->getTrueValue());
      Value *FalseSaved = getSavedStackPtr(SI->getFalseValue());
      IRBuilder<> B(SI);
      return SavedStackPtrs[SI] = B.CreateSelect(SI->getCondition(), TrueSaved,
                                                 FalseSaved, "palisade.saved");
    }
    if (isa<UndefValue>(Token))
      return PoisonValue::get(PtrTy);
    report_fatal_error(
        Twine("Palisade stack split: unsupported stackrestore, ") +
            "e.g. a VLA at -O0, in function '" + F.getName() + "'",
        false);
  }

public:
  StackSplitter(Function &F)
      : F(F), DL(F.getDataLayout()),
        PtrTy(PointerType::getUnqual(F.getContext())),
        IntPtrTy(DL.getIntPtrType(F.getContext())) {}

  // Collect the ordinary objects and every point that moves the unsafe stack
  // pointer.
  bool collect() {
    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAddressSpace() == ProtectedAddressSpace)
          continue;
        if (AI->isStaticAlloca())
          addObject(AI, AI->getAllocationSize(DL)->getFixedValue(),
                    AI->getAlign());
        else
          DynamicAllocas.push_back(AI);
      } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
        Returns.push_back(RI);
      } else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        if (II->getIntrinsicID() == Intrinsic::stacksave)
          StackSaves.push_back(II);
        else if (II->getIntrinsicID() == Intrinsic::stackrestore)
          StackRestores.push_back(II);
      }
    }

    // The callee may write its byval copy, so the copy moves as well.
    for (Argument &Arg : F.args()) {
      if (!Arg.hasByValAttr())
        continue;
      Type *Ty = Arg.getParamByValType();
      addObject(
          &Arg, DL.getTypeAllocSize(Ty).getFixedValue(),
          std::max(DL.getABITypeAlign(Ty), Arg.getParamAlign().valueOrOne()));
    }

    return !Objects.empty() || !DynamicAllocas.empty();
  }

  void split(GlobalVariable *GV) {
    UnsafeStackPtr = GV;

    // Reserve the static frame below the incoming unsafe stack pointer, before
    // any original instruction.
    IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
    Value *Incoming = loadUnsafeStackPtr(B, "palisade.incoming");
    Value *Base = Incoming;
    if (FrameAlign > StackAlignment)
      Base = alignDown(B, Base, FrameAlign);
    FrameSize = alignTo(FrameSize, StackAlignment);
    if (FrameSize)
      storeUnsafeStackPtr(
          B, B.CreatePtrAdd(
                 Base, ConstantInt::getSigned(IntPtrTy, -int64_t(FrameSize))));

    // Replace Static allocas.
    SmallVector<std::pair<AllocaInst *, Value *>> Replacements;
    for (auto [Object, Offset] : Objects) {
      Value *Address = B.CreatePtrAdd(
          Base, ConstantInt::getSigned(IntPtrTy, -int64_t(Offset)),
          Object->getName() + ".ordinary");
      if (auto *AI = dyn_cast<AllocaInst>(Object)) {
        Replacements.push_back({AI, Address});
        continue;
      }

      // Objects is a byval argument.
      // Spill only happen on protected stack, so we copy the memory of byval
      // into the ordinary stack.
      auto *Arg = cast<Argument>(Object);
      uint64_t Size =
          DL.getTypeAllocSize(Arg->getParamByValType()).getFixedValue();
      Arg->replaceAllUsesWith(Address);
      B.CreateMemCpy(Address, commonAlignment(FrameAlign, Offset), Arg,
                     Arg->getParamAlign(), ConstantInt::get(IntPtrTy, Size));
    }

    for (auto [AI, Address] : Replacements)
      replaceAlloca(AI, Address);

    // A dynamic allocation moves the unsafe stack pointer down where it
    // happens.
    for (AllocaInst *AI : DynamicAllocas) {
      IRBuilder<> At(AI);
      Value *Size = At.CreateAllocationSize(IntPtrTy, AI);
      Value *Top = At.CreatePtrAdd(loadUnsafeStackPtr(At, "palisade.top"),
                                   At.CreateNeg(Size));
      Top = alignDown(At, Top, std::max(StackAlignment, AI->getAlign()));
      storeUnsafeStackPtr(At, Top);
      replaceAlloca(AI, Top);
    }

    // Each stackrestore also restores the unsafe stack pointer saved at its
    // stacksave.
    if (!DynamicAllocas.empty()) {
      for (IntrinsicInst *Save : StackSaves) {
        IRBuilder<> At(Save->getNextNode());
        SavedStackPtrs[Save] = loadUnsafeStackPtr(At, "palisade.saved");
      }
      for (IntrinsicInst *Restore : StackRestores) {
        Value *Saved = getSavedStackPtr(Restore->getArgOperand(0));
        IRBuilder<> At(Restore);
        storeUnsafeStackPtr(At, Saved);
      }
    }

    // Every return restores the incoming unsafe stack pointer.
    for (ReturnInst *RI : Returns) {
      IRBuilder<> At(RI);
      storeUnsafeStackPtr(At, Incoming);
    }
  }
};

} // namespace

// Get the GV for unsafe pointer, create if its not in the current module.
static GlobalVariable *getUnsafeStackPtr(Module &M) {
  GlobalVariable *GV = M.getNamedGlobal(UnsafeStackPtrName);

  // Create declaration for unsafe stack pointer.
  if (!GV)
    GV = new GlobalVariable(M, PointerType::getUnqual(M.getContext()),
                            /*isConstant=*/false, GlobalValue::ExternalLinkage,
                            nullptr, UnsafeStackPtrName, nullptr,
                            GlobalVariable::NotThreadLocal,
                            ProtectedAddressSpace);

  // Define the unsafe stack pointer.
  if (GV->isDeclaration()) {
    GV->setInitializer(
        M.getOrInsertGlobal(StackTopName, Type::getInt8Ty(M.getContext())));

    // UnsafeStackPtr has weak linkage and be grouped among object files.
    GV->setLinkage(GlobalValue::WeakAnyLinkage);
    GV->setComdat(M.getOrInsertComdat(UnsafeStackPtrName));

    GV->setSection(".data.palisade");
  }
  return GV;
}

PreservedAnalyses PalisadeStackSplitPass::run(Module &M,
                                              ModuleAnalysisManager &AM) {
  GlobalVariable *UnsafeStackPtr = nullptr;
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // Find function that needs to be split.
    StackSplitter Splitter(F);
    if (!Splitter.collect())
      continue;

    if (!UnsafeStackPtr)
      UnsafeStackPtr = getUnsafeStackPtr(M);

    // Split stack allocations.
    Splitter.split(UnsafeStackPtr);
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
