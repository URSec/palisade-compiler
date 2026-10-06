//===- PalisadeGlobalSegregation.cpp - Protected globals ------------------===//
//
//     Segregating globals with protected type qualifier.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PalisadeGlobalSegregation.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace {

// Clang maps __protected object storage to address space 200.
constexpr unsigned ProtectedAddressSpace = 200;


// Uninitialized or zero-initialized GV goes to .bss
// Aggregate check its sub-elements
bool isNullOrUndef(const Constant *C) {
  if (C->isNullValue() || isa<UndefValue>(C))
    return true;
  if (!isa<ConstantAggregate>(C))
    return false;
  for (const Value *Operand : C->operand_values())
    if (!isNullOrUndef(cast<Constant>(Operand)))
      return false;
  return true;
}


// Read-only constant goes to .rodata
// Mutable (non-const) check if its initialized with non-zero value
StringRef getPalisadeSection(const GlobalVariable &GV) {
  if (GV.isConstant())
    return ".rodata.palisade";
  if (isNullOrUndef(GV.getInitializer()))
    return ".bss.palisade";
  return ".data.palisade";
}

} // namespace

PreservedAnalyses
PalisadeGlobalSegregationPass::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;

  for (GlobalVariable &GV : M.globals()) {
    // skip ordinary var or declarations
    if (GV.getAddressSpace() != ProtectedAddressSpace ||
        GV.isDeclarationForLinker())
      continue;

    StringRef Section = getPalisadeSection(GV);
    if (GV.getSection() != Section) {
      GV.setSection(Section);
      Changed = true;
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
