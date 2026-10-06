//===- PalisadeGlobalSegregation.h - Protected globals ----------*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEGLOBALSEGREGATION_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEGLOBALSEGREGATION_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

/// Place global variables defined __protected type qualifier into
/// dedicated ELF sections, so a linker script can group them into one region:
///   .rodata.palisade  constants
///   .bss.palisade     mutable objects that are zero or uninitialized
///   .data.palisade    initialized global and static variables
class PalisadeGlobalSegregationPass
    : public RequiredPassInfoMixin<PalisadeGlobalSegregationPass> {
public:
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEGLOBALSEGREGATION_H
