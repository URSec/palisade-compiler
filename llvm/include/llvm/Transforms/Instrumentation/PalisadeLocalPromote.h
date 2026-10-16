//===- PalisadeLocalPromote.h - Promote locals ------------------*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PALISADELOCALPROMOTE_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PALISADELOCALPROMOTE_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

/// Analyze allocas on ordinary address space, promote them to protected
/// address space if we can identify their accesses always safe.
class PalisadeLocalPromotePass
    : public RequiredPassInfoMixin<PalisadeLocalPromotePass> {
public:
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_PALISADELOCALPROMOTE_H
