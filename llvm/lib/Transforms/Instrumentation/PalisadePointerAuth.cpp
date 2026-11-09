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
// For global intialized pointer, create a constructor to sign the pointer
// so that it can be authenticate at runtime.
//
// Atomics and MVE are currently not supported.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PalisadePointerAuth.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsARM.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
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

[[noreturn]] void reportUnsupported(const GlobalVariable &GV,
                                    const Twine &Reason) {
  report_fatal_error(Twine("Palisade pointer auth: ") + Reason +
                         ", in global '" + GV.getName() + "'",
                     false);
}

class PointerAuth {
  Module &M;
  const DataLayout &DL;
  IntegerType *Int32Ty;
  SmallVector<StoreInst *> Stores;
  SmallVector<LoadInst *> Loads;
  // Non-null protected pointers in global initializers, by global and offset.
  SmallVector<std::pair<GlobalVariable *, uint64_t>> InitializedGlobalPtrs;

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
    // Object needs room for a pointer and its PAC at Offset.
    auto NeedRoom = [&](Value *Object, uint64_t Offset) {
      uint64_t End = Offset + PointerPairSize;
      if (End > *getObjectSize(Object)) {
        uint64_t &NewSize = NewSizes[Object];
        NewSize = std::max(NewSize, End);
      }
    };
    auto AddAccess = [&](Instruction *I) {
      Value *PointerAddr = getLoadStorePointerOperand(I);
      APInt Offset(DL.getIndexTypeSizeInBits(PointerAddr->getType()), 0);
      Value *Base = PointerAddr->stripAndAccumulateConstantOffsets(
          DL, Offset, /*AllowNonInbounds=*/true);
      std::optional<uint64_t> Size = getObjectSize(Base);
      if (Size && !Offset.isNegative()) {
        NeedRoom(Base, Offset.getZExtValue());
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
    for (auto [GV, Offset] : InitializedGlobalPtrs)
      NeedRoom(GV, Offset);

    for (auto [Object, NewSize] : NewSizes)
      grow(Object, NewSize);
  }

  // Find the non-null protected pointers.
  void findInitializedGlobalPtrs(GlobalVariable &GV, Constant *C,
                                 uint64_t Offset) {
    // Null, zero, undef, numbers and byte strings hold no pointer to sign.
    if (isa<ConstantData>(C))
      return;

    if (isProtectedPointer(C->getType())) {
      // The PAC goes in the word after the pointer and initialized to be
      // 0 in clang.
      Constant *PACWord = ConstantFoldLoadFromConst(
          GV.getInitializer(), Int32Ty, APInt(64, Offset + PACOffset), DL);

      if (!PACWord || !(PACWord->isNullValue() || isa<UndefValue>(PACWord)))
        reportUnsupported(GV, "protected pointer without room for its PAC");

      InitializedGlobalPtrs.push_back({&GV, Offset});
      return;
    }

    // initialized aggregate
    if (auto *ST = dyn_cast<StructType>(C->getType())) {
      const StructLayout *Layout = DL.getStructLayout(ST);
      for (unsigned I = 0, E = ST->getNumElements(); I != E; ++I)
        findInitializedGlobalPtrs(
            GV, C->getAggregateElement(I),
            Offset + Layout->getElementOffset(I).getFixedValue());

    } else if (auto *AT = dyn_cast<ArrayType>(C->getType())) {
      uint64_t Stride = DL.getTypeAllocSize(AT->getElementType());
      for (uint64_t I = 0, E = AT->getNumElements(); I != E; ++I)
        findInitializedGlobalPtrs(GV, C->getAggregateElement(I),
                                  Offset + I * Stride);
    }
  }

  Value *getPACAddress(IRBuilder<> &B, Value *PointerAddr) {
    return B.CreateInBoundsPtrAdd(PointerAddr, B.getInt32(PACOffset),
                                  "palisade.pac.addr");
  }

  // Sign the pointer with its address.
  Value *createPAC(IRBuilder<> &B, Value *Ptr, Value *PointerAddr) {
    Value *Modifier =
        B.CreatePtrToInt(PointerAddr, Int32Ty, "palisade.modifier");
    return B.CreateIntrinsic(Intrinsic::arm_protected_sign, {Ptr, Modifier}, {},
                             "palisade.pac");
  }

  // Store the PAC after the pointer.
  // A null pointer is not signed.
  void signStore(StoreInst *SI) {
    Value *Ptr = SI->getValueOperand();
    if (isa<ConstantPointerNull>(Ptr))
      return;

    Value *PointerAddr = SI->getPointerOperand();
    IRBuilder<> B(SI);
    Value *PAC = createPAC(B, Ptr, PointerAddr);
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

  // Sign the pointers globals are initialized with, in a constructor that runs
  // at boot before other constructors.
  void signInitializedGlobalPtrs() {
    if (InitializedGlobalPtrs.empty())
      return;

    LLVMContext &Ctx = M.getContext();
    Function *F = Function::createWithDefaultAttr(
        FunctionType::get(Type::getVoidTy(Ctx), /*isVarArg=*/false),
        GlobalValue::InternalLinkage, DL.getProgramAddressSpace(),
        "palisade.sign.globals", &M);
    F->setDoesNotThrow();
    IRBuilder<> B(BasicBlock::Create(Ctx, "", F));
    for (auto [GV, Offset] : InitializedGlobalPtrs) {
      // A constant global lives in flash, where the PAC can't be written.
      if (GV->isConstant()) {
        if (GV->hasSection())
          reportUnsupported(*GV, "protected pointer in a constant global with "
                                 "an explicit section");
        GV->setConstant(false);
      }

      // Sign the pointer in memory
      Value *PointerAddr = B.CreateInBoundsPtrAdd(GV, B.getInt32(Offset));
      Align PointerAlign = commonAlignment(GV->getPointerAlignment(DL), Offset);
      Value *Ptr = B.CreateAlignedLoad(B.getPtrTy(ProtectedAddressSpace),
                                       PointerAddr, PointerAlign);
      B.CreateAlignedStore(createPAC(B, Ptr, PointerAddr),
                           getPACAddress(B, PointerAddr),
                           commonAlignment(PointerAlign, PACOffset));
    }
    B.CreateRetVoid();
    appendToGlobalCtors(M, F, /*Priority=*/0);
  }

public:
  PointerAuth(Module &M)
      : M(M), DL(M.getDataLayout()), Int32Ty(Type::getInt32Ty(M.getContext())) {
  }

  void collect(GlobalVariable &GV) {
    if (!GV.isDeclarationForLinker() &&
        GV.getAddressSpace() != ProtectedAddressSpace)
      findInitializedGlobalPtrs(GV, GV.getInitializer(), 0);
  }

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
    if (Stores.empty() && Loads.empty() && InitializedGlobalPtrs.empty())
      return false;

    growObjects();
    for (StoreInst *SI : Stores)
      signStore(SI);
    for (LoadInst *LI : Loads)
      authLoad(LI);
    signInitializedGlobalPtrs();
    return true;
  }
};

} // namespace

PreservedAnalyses PalisadePointerAuthPass::run(Module &M,
                                               ModuleAnalysisManager &AM) {
  PointerAuth Auth(M);
  for (GlobalVariable &GV : M.globals())
    Auth.collect(GV);
  for (Function &F : M)
    if (!F.isDeclaration())
      Auth.collect(F);

  return Auth.instrument() ? PreservedAnalyses::none()
                           : PreservedAnalyses::all();
}
