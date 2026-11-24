// REQUIRES: arm-registered-target
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade-pac -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang --target=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -fpalisade-pac -O2 -S %s -o - | FileCheck %s --check-prefix=ASM
// RUN: %clang --target=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -fpalisade -O2 -S %s -o - | FileCheck %s --check-prefix=NOPAC

typedef int (*fn_t)(int);

int foo(int x);
int bar(int x);

fn_t ordinary_fn = foo;
fn_t const const_table[2] = {foo, bar};
int (*__protected protected_fn)(int) = foo;

// IR-DAG: @ordinary_fn = {{.*}}global { ptr addrspace(200), i32 } { ptr addrspace(200) addrspacecast (ptr @foo to ptr addrspace(200)), i32 0 }
// IR-DAG: @const_table = {{.*}}constant [2 x { ptr addrspace(200), i32 }] [{ ptr addrspace(200), i32 } { ptr addrspace(200) addrspacecast (ptr @foo to ptr addrspace(200)), i32 0 }, { ptr addrspace(200), i32 } { ptr addrspace(200) addrspacecast (ptr @bar to ptr addrspace(200)), i32 0 }]
// IR-DAG: @protected_fn = {{.*}}addrspace(200) global { ptr addrspace(200), i32 } { ptr addrspace(200) addrspacecast (ptr @foo to ptr addrspace(200)), i32 0 }

// ASM-LABEL: store_ordinary:
// ASM:       pacg
// ASM:       strt
// ASM:       strt
// NOPAC-LABEL: store_ordinary:
// NOPAC-NOT:   pacg
// NOPAC:       strt
// NOPAC:       bx lr
void store_ordinary(fn_t f) { ordinary_fn = f; }

// ASM-LABEL: call_ordinary:
// ASM:       ldrd
// ASM:       cmp
// ASM-NEXT:  it ne
// ASM-NEXT:  autgne
// ASM:       bx
// NOPAC-LABEL: call_ordinary:
// NOPAC-NOT:   autg
// NOPAC:       ldr
// NOPAC:       bx
int call_ordinary(int x) { return ordinary_fn(x); }

// ASM-LABEL: call_const:
// ASM:       autgne
// ASM:       bx
int call_const(int i, int x) { return const_table[i](x); }

// Function pointers stored in protected memory do not need PAC.
// ASM-LABEL: store_protected:
// ASM-NOT:   pacg
// ASM-NOT:   strt
// ASM:       str
// ASM:       bx lr
void store_protected(fn_t f) { protected_fn = f; }

// ASM-LABEL: call_protected:
// ASM-NOT:   autg
// ASM:       ldr
// ASM:       bx
int call_protected(int x) { return protected_fn(x); }

// Structs containing function pointers are copied field-by-field so each PAC is
// authenticated at the source and re-signed for the destination.
struct ops {
  int id;
  fn_t cb;
};

// ASM-LABEL: copy_ops:
// ASM:       autgne
// ASM:       pacg
// ASM:       bx lr
void copy_ops(struct ops *dst, struct ops *src) { *dst = *src; }

// ASM-LABEL: palisade.sign.globals:
// ASM:       pacg
// ASM:       strt
