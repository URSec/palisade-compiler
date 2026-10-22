// REQUIRES: arm-registered-target
// RUN: %clang --target=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -fpalisade -O2 -S %s -o - | FileCheck %s
// RUN: %clang --target=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -fpalisade -O0 -S %s -o - | FileCheck %s --check-prefix=O0

// -O0 runs no Palisade pass.
// O0-NOT: strt
// O0-NOT: __palisade_unsafe_stack_ptr

void consume(char *);

// The escaping array moves to the unsafe stack.
// CHECK-LABEL: locals:
// CHECK:      movw r4, :lower16:__palisade_unsafe_stack_ptr
// CHECK:      ldr r5, [r4]
// CHECK-NEXT: sub.w r0, r5, #16
// CHECK-NEXT: str r0, [r4]
// CHECK-NEXT: bl consume
// CHECK-NEXT: str r5, [r4]
void locals(void) {
  char buffer[16];
  consume(buffer);
}

// Ordinary stores are unprivileged; protected stores stay privileged.
// CHECK-LABEL: domains:
// CHECK:      strt r2, [r0]
// CHECK-NEXT: str r2, [r1]
void domains(int *p, __protected int *q, int value) {
  *p = value;
  *q = value;
}

// Protected globals move to Palisade sections.
// CHECK:      .section .data.palisade,"aw",%progbits
// CHECK:      protected_global:
__protected int protected_global = 7;
