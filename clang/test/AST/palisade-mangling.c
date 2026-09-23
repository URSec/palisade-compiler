// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -ast-dump=json %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fpalisade -std=c11 -ast-dump=json %s | FileCheck %s --check-prefix=MS

// C overloadable functions exercise both manglers without enabling Palisade
// in C++ or requiring LLVM address-space lowering.
void __attribute__((overloadable)) select(int *);
void __attribute__((overloadable)) select(__protected int *);
// ITANIUM: "mangledName": "_Z6selectPi"
// ITANIUM: "mangledName": "_Z6selectPU11__protectedi"
// MS: "mangledName": "?select@@$$J0YAXPEAH@Z"
// MS: "mangledName": "?select@@$$J0YAXPEAU?$_AS__protected@$$CAH@__clang@@@Z"
