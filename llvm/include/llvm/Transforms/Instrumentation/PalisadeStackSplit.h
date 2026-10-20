//===- PalisadeStackSplit.h - Separate ordinary stack -----------*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PALISADESTACKSPLIT_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PALISADESTACKSPLIT_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

// Identify the locals with ordinary address space, and move them to
// a separated stack.
class PalisadeStackSplitPass
    : public RequiredPassInfoMixin<PalisadeStackSplitPass> {
public:
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_PALISADESTACKSPLIT_H
