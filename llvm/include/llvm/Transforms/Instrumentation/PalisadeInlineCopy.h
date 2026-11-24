//===- PalisadeInlineCopy.h - Inline protected copies -----------*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEINLINECOPY_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEINLINECOPY_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

/// Expand llvm.memcpy/llvm.memmove/llvm.memset into inlined store instructions
/// instead of calling C library.
class PalisadeInlineCopyPass
    : public RequiredPassInfoMixin<PalisadeInlineCopyPass> {
public:
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEINLINECOPY_H
