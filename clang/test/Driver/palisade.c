/// Palisade feature configuration: the -fpalisade opt-in flag.

/// The driver forwards the flag to cc1.
// RUN: %clang --target=armv8.1m.main-none-eabi -fpalisade -### -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FLAG
// FLAG: "-fpalisade"

/// The feature is off by default
// RUN: %clang --target=armv8.1m.main-none-eabi -### -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=OFF
// OFF-NOT: "-fpalisade"

/// The qualifier is a C only extension.
// RUN: not %clang --target=armv8.1m.main-none-eabi -fpalisade -x c++ -fsyntax-only %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CXX
// CXX: invalid argument '-fpalisade' not allowed with 'C++'

/// Accepted targets. Armv8.1-M, either arm or thumb
/// For testing, X86 is also permitted
// RUN: %clang --target=armv8.1m.main-none-eabi -fpalisade -fsyntax-only %s
// RUN: %clang --target=thumbv8.1m.main-none-eabi -fpalisade -fsyntax-only %s
// RUN: %clang --target=x86_64-linux-gnu -fpalisade -fsyntax-only %s

/// Reject 64-bit Arm
// RUN: not %clang --target=aarch64-none-elf -fpalisade -fsyntax-only %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=AARCH64
// AARCH64: unsupported option '-fpalisade' for target 'aarch64-unknown-none-elf'

/// Reject riscv
// RUN: not %clang --target=riscv32-none-elf -fpalisade -fsyntax-only %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=TARGET
// TARGET: unsupported option '-fpalisade' for target 'riscv32-unknown-none-elf'

/// Enabling the feature alone changes no existing behaviour.
// RUN: %clang --target=armv8.1m.main-none-eabi -fpalisade -fsyntax-only -Xclang -verify %s
// expected-no-diagnostics
int ordinary(void) { return 0; }
