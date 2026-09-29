// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,ARM --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,X86 --enable-var-scope --implicit-check-not=addrspacecast

// A top-level parameter qualifier controls its local storage, not its ABI.
void consume(__protected int *p);
void consume_slot(__protected int *__protected *p);

// CHECK-LABEL: define{{.*}} void @scalar_parameter(i32 noundef %value)
// CHECK: %value.addr = alloca i32, align 4, addrspace(200)
// CHECK: store i32 %value, ptr addrspace(200) %value.addr
// CHECK: call void @consume(ptr addrspace(200) {{.*}}%value.addr)
void scalar_parameter(__protected int value) { consume(&value); }

// CHECK-LABEL: define{{.*}} void @pointer_parameter(ptr addrspace(200) noundef %value)
// CHECK: %value.addr = alloca ptr addrspace(200), align {{[0-9]+}}, addrspace(200)
// CHECK: store ptr addrspace(200) %value, ptr addrspace(200) %value.addr
// CHECK: call void @consume_slot(ptr addrspace(200) {{.*}}%value.addr)
void pointer_parameter(__protected int *__protected value) {
  consume_slot(&value);
}

struct small { int value; };
void consume_small(__protected struct small *p);

// Direct aggregate arguments are reconstructed into protected allocations.
// CHECK-LABEL: define{{.*}} void @small_parameter(
// CHECK: [[OBJECT:%.*]] = alloca %struct.small, align 4, addrspace(200)
// CHECK: [[FIELD:%.*]] = getelementptr inbounds{{.*}} %struct.small, ptr addrspace(200) [[OBJECT]], i32 0, i32 0
// ARM: store [1 x i32] {{.*}}, ptr addrspace(200) [[FIELD]]
// X86: store i32 {{.*}}, ptr addrspace(200) [[FIELD]]
// CHECK: call void @consume_small(ptr addrspace(200) {{.*}}[[OBJECT]])
void small_parameter(__protected struct small value) { consume_small(&value); }

struct large { int values[32]; };
void consume_large(__protected struct large *p);

// The ABI passes this by value through an ordinary pointer. Copy the contents into
// protected local storage, rather than casting the caller's ordinary address.
// CHECK-LABEL: define{{.*}} void @large_parameter(ptr noundef byval(%struct.large)
// CHECK: [[OBJECT:%.*]] = alloca %struct.large, align {{[0-9]+}}, addrspace(200)
// CHECK: call void @llvm.memcpy.p200.p0.i{{32|64}}(ptr addrspace(200) {{.*}}[[OBJECT]], ptr {{.*}}, i{{32|64}} 128, i1 false)
// CHECK: call void @consume_large(ptr addrspace(200) {{.*}}[[OBJECT]])
void large_parameter(__protected struct large value) { consume_large(&value); }

// Ordinary aggregate parameters continue to use ordinary storage.
// CHECK-LABEL: define{{.*}} i32 @ordinary_parameter(
// CHECK-NOT: addrspace(200)
// CHECK: ret i32
int ordinary_parameter(struct large value) { return value.values[0]; }
