//===- PalisadePointerAuth.h - Protected pointer auth -----------*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEPOINTERAUTH_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEPOINTERAUTH_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

/// Sign a protected pointer when it is stored to ordinary memory, and
/// authenticate it when it is loaded back, using the address of pointer
/// as modifier (&p). Protected pointers in global initializers are signed at
/// boot.
class PalisadePointerAuthPass
    : public RequiredPassInfoMixin<PalisadePointerAuthPass> {
public:
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_PALISADEPOINTERAUTH_H
