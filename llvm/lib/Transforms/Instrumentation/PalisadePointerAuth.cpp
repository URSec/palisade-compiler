//===- PalisadePointerAuth.cpp - Protected pointer auth -------------------===//
//
//     Signing and authenticating protected pointers in ordinary memory.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Signing pointer with PAC when pointer stores into memory, authenticating
// pointer with its PAC when load from memory.
// Only pointer load/store into ordinary memory is PACed, pointer load/store
// into protected memory are trusted.
// Run this pass at very late end so that SORA can remove many unused PAC
// slot for a fat pointer.
//
// Atomics and MVE are currently not supported.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PalisadePointerAuth.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsARM.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace llvm;

namespace {

constexpr unsigned ProtectedAddressSpace = 200;

// Fat pointer layout: {ptr, PAC}
constexpr uint64_t PACOffset = 4;
constexpr uint64_t PointerPairSize = 8;

bool isProtectedPointer(Type *Ty) {
  return Ty->isPointerTy() &&
         Ty->getPointerAddressSpace() == ProtectedAddressSpace;
}

// A protected pointer loaded from or stored to ordinary memory needs a PAC.
bool needsPAC(Instruction &I, Type *ValueTy) {
  return !I.isAtomic() && isProtectedPointer(ValueTy) &&
         getLoadStorePointerOperand(&I)->getType()->getPointerAddressSpace() !=
             ProtectedAddressSpace;
}

[[noreturn]] void reportUnsupported(const Instruction &I, const Twine &Reason) {
  report_fatal_error(Twine("Palisade pointer auth: ") + Reason +
                         ", in function '" + I.getFunction()->getName() + "'",
                     false);
}

class PointerAuth {
  const DataLayout &DL;
  IntegerType *Int32Ty;
  SmallVector<StoreInst *> Stores;
  SmallVector<LoadInst *> Loads;

  // The size of a local or global definition.
  std::optional<uint64_t> getObjectSize(Value *Object) {
    if (auto *AI = dyn_cast<AllocaInst>(Object); AI && !AI->isArrayAllocation())
      return DL.getTypeAllocSize(AI->getAllocatedType()).getFixedValue();
    if (auto *GV = dyn_cast<GlobalVariable>(Object); GV && GV->hasInitializer())
      return DL.getTypeAllocSize(GV->getValueType()).getFixedValue();
    return std::nullopt;
  }

  // Pad the object at its end to NewSize bytes.
  void grow(Value *Object, uint64_t NewSize) {
    LLVMContext &Ctx = Object->getContext();
    Type *Padding =
        ArrayType::get(Type::getInt8Ty(Ctx), NewSize - *getObjectSize(Object));
    if (auto *AI = dyn_cast<AllocaInst>(Object)) {
      AI->setAllocatedType(
          StructType::get(Ctx, {AI->getAllocatedType(), Padding}));
      return;
    }
    auto *GV = cast<GlobalVariable>(Object);
    auto *Ty = StructType::get(Ctx, {GV->getValueType(), Padding});
    GV->replaceInitializer(ConstantStruct::get(
        Ty, {GV->getInitializer(), Constant::getNullValue(Padding)}));
  }

  // GlobalOpt and SROA shrink an object for unused PAC.
  // Grow such objects back so each protected pointer has room for its PAC.
  void growObjects() {
    DenseMap<Value *, uint64_t> NewSizes;
    auto AddAccess = [&](Instruction *I) {
      Value *PointerAddr = getLoadStorePointerOperand(I);
      APInt Offset(DL.getIndexTypeSizeInBits(PointerAddr->getType()), 0);
      Value *Base = PointerAddr->stripAndAccumulateConstantOffsets(
          DL, Offset, /*AllowNonInbounds=*/true);
      std::optional<uint64_t> Size = getObjectSize(Base);
      if (Size && !Offset.isNegative()) {
        uint64_t End = Offset.getZExtValue() + PointerPairSize;
        if (End > *Size) {
          uint64_t &NewSize = NewSizes[Base];
          NewSize = std::max(NewSize, End);
        }
        return;
      }

      // Object size should match Clang defined size.
      Size = getObjectSize(getUnderlyingObject(Base));
      if (Size && *Size < PointerPairSize)
        reportUnsupported(*I, "protected pointer in an object too small for "
                              "its PAC");
    };
    for (StoreInst *SI : Stores)
      AddAccess(SI);
    for (LoadInst *LI : Loads)
      AddAccess(LI);

    for (auto [Object, NewSize] : NewSizes)
      grow(Object, NewSize);
  }

  Value *getPACAddress(IRBuilder<> &B, Value *PointerAddr) {
    return B.CreateInBoundsPtrAdd(PointerAddr, B.getInt32(PACOffset),
                                  "palisade.pac.addr");
  }

  // Store the PAC after the pointer.
  // A null pointer is not signed.
  void signStore(StoreInst *SI) {
    Value *Ptr = SI->getValueOperand();
    if (isa<ConstantPointerNull>(Ptr))
      return;

    Value *PointerAddr = SI->getPointerOperand();
    IRBuilder<> B(SI);
    Value *Modifier =
        B.CreatePtrToInt(PointerAddr, Int32Ty, "palisade.modifier");
    Value *PAC = B.CreateIntrinsic(Intrinsic::arm_protected_sign,
                                   {Ptr, Modifier}, {}, "palisade.pac");
    B.SetInsertPoint(SI->getNextNode());
    B.CreateAlignedStore(PAC, getPACAddress(B, PointerAddr),
                         commonAlignment(SI->getAlign(), PACOffset),
                         SI->isVolatile());
  }

  // Check the pointer using PAC.
  void authLoad(LoadInst *LI) {
    Value *PointerAddr = LI->getPointerOperand();
    IRBuilder<> B(LI->getNextNode());
    Value *PAC = B.CreateAlignedLoad(Int32Ty, getPACAddress(B, PointerAddr),
                                     commonAlignment(LI->getAlign(), PACOffset),
                                     LI->isVolatile(), "palisade.pac");
    Value *Modifier =
        B.CreatePtrToInt(PointerAddr, Int32Ty, "palisade.modifier");
    Value *Checked =
        B.CreateIntrinsic(Intrinsic::arm_protected_auth, {LI, PAC, Modifier},
                          {}, "palisade.auth");
    LI->replaceUsesWithIf(Checked,
                          [Checked](Use &U) { return U.getUser() != Checked; });
  }

public:
  PointerAuth(Module &M)
      : DL(M.getDataLayout()), Int32Ty(Type::getInt32Ty(M.getContext())) {}

  void collect(Function &F) {
    for (Instruction &I : instructions(F)) {
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (needsPAC(*SI, SI->getValueOperand()->getType()))
          Stores.push_back(SI);
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (needsPAC(*LI, LI->getType()))
          Loads.push_back(LI);
      }
    }
  }

  bool instrument() {
    if (Stores.empty() && Loads.empty())
      return false;

    growObjects();
    for (StoreInst *SI : Stores)
      signStore(SI);
    for (LoadInst *LI : Loads)
      authLoad(LI);
    return true;
  }
};

} // namespace

PreservedAnalyses PalisadePointerAuthPass::run(Module &M,
                                               ModuleAnalysisManager &AM) {
  PointerAuth Auth(M);
  for (Function &F : M)
    if (!F.isDeclaration())
      Auth.collect(F);

  return Auth.instrument() ? PreservedAnalyses::none()
                           : PreservedAnalyses::all();
}
