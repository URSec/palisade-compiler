// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,O0 --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,O0 --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,OPT --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,OPT --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -DRETURN_QUALIFIER=__protected -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,O0 --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -DRETURN_QUALIFIER=__protected -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,O0 --enable-var-scope --implicit-check-not=addrspacecast

// Qualifying the function's return type does not change its ABI return storage.
#ifndef RETURN_QUALIFIER
#define RETURN_QUALIFIER
#endif

struct small { int value; };
struct large { int values[32]; };
void consume_small(__protected struct small *p);
void consume_large(__protected struct large *p);
void consume_ordinary(struct large *p);

// A register return must read the protected local after the call. Reusing an
// ordinary return-slot allocation and casting its address would lose protection.
// CHECK-LABEL: define{{.*}} i32 @protected_small(
// CHECK: [[LOCAL:%.*]] = alloca %struct.small, align 4, addrspace(200)
// O0: call void @llvm.memset.p200.i{{32|64}}(ptr addrspace(200) {{.*}}[[LOCAL]], i8 0,
// OPT: store i32 0, ptr addrspace(200) [[LOCAL]]
// CHECK: call void @consume_small(ptr addrspace(200) {{.*}}[[LOCAL]])
// O0: call void @llvm.memcpy.p0.p200.i{{32|64}}(ptr {{.*}}, ptr addrspace(200) {{.*}}[[LOCAL]], i{{32|64}} 4,
// O0: ret i32
// OPT: [[VALUE:%.*]] = load i32, ptr addrspace(200) [[LOCAL]]
// OPT: ret i32 [[VALUE]]
RETURN_QUALIFIER struct small protected_small(void) {
  __protected struct small value = {0};
  consume_small(&value);
  return value;
}

// An indirect return must copy the protected local into the caller's ordinary
// sret buffer, after the call that may modify the local.
// CHECK-LABEL: define{{.*}} void @protected_large(
// CHECK-SAME: ptr {{.*}}sret(%struct.large) {{.*}}[[RESULT:%[^,) ]+]])
// CHECK: [[LOCAL:%.*]] = alloca %struct.large, align {{[0-9]+}}, addrspace(200)
// CHECK: call void @llvm.memset.p200.i{{32|64}}(ptr addrspace(200) {{.*}}[[LOCAL]], i8 0,
// CHECK: call void @consume_large(ptr addrspace(200) {{.*}}[[LOCAL]])
// CHECK: call void @llvm.memcpy.p0.p200.i{{32|64}}(ptr {{.*}}[[RESULT]], ptr addrspace(200) {{.*}}[[LOCAL]], i{{32|64}} 128, i1 false)
// CHECK: ret void
RETURN_QUALIFIER struct large protected_large(void) {
  __protected struct large value = {0};
  consume_large(&value);
  return value;
}

// Ordinary locals still use NRVO with no separate allocation or return copy.
// CHECK-LABEL: define{{.*}} void @ordinary_large(
// CHECK-SAME: ptr {{.*}}sret(%struct.large) {{.*}}[[RESULT:%[^,) ]+]])
// CHECK-NOT: alloca
// CHECK: call void @llvm.memset.p0.i{{32|64}}(ptr {{.*}}[[RESULT]], i8 0,
// CHECK-NOT: alloca
// CHECK: call void @consume_ordinary(ptr {{.*}}[[RESULT]])
// CHECK-NOT: @llvm.memcpy
// CHECK: ret void
struct large ordinary_large(void) {
  struct large value = {0};
  consume_ordinary(&value);
  return value;
}
