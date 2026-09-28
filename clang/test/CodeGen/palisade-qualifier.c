// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,O0 --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,O0 --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,OPT --enable-var-scope --implicit-check-not=addrspacecast
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,OPT --enable-var-scope --implicit-check-not=addrspacecast

// Address space 200 identifies protected storage. Pointer values and the
// objects holding those pointers have independent address spaces.
int ordinary;
__protected int protected_zero;
__protected int protected_value = 7;
extern __protected int protected_external;
int *p = &ordinary;
__protected int *q = &protected_value;
int *__protected r = &ordinary;
__protected int *__protected t = &protected_value;
__protected int values[4];
int (*__protected callback)(void);
__protected int *literal = &(__protected int){11};

// CHECK-DAG: @ordinary = {{.*}}global i32 0
// CHECK-DAG: @protected_zero = {{.*}}addrspace(200) global i32 0
// CHECK-DAG: @protected_value = {{.*}}addrspace(200) global i32 7
// CHECK-DAG: @protected_external = external {{.*}}addrspace(200) global i32
// CHECK-DAG: @p = {{.*}}global ptr @ordinary
// CHECK-DAG: @q = {{.*}}global ptr addrspace(200) @protected_value
// CHECK-DAG: @r = {{.*}}addrspace(200) global ptr @ordinary
// CHECK-DAG: @t = {{.*}}addrspace(200) global ptr addrspace(200) @protected_value
// CHECK-DAG: @values = {{.*}}addrspace(200) global [4 x i32] zeroinitializer
// CHECK-DAG: @callback = {{.*}}addrspace(200) global ptr null
// CHECK-DAG: @.compoundliteral = internal addrspace(200) global i32 11
// CHECK-DAG: @literal = {{.*}}global ptr addrspace(200) @.compoundliteral
// CHECK-DAG: @static_storage.value = internal addrspace(200) global i32 9

// CHECK-LABEL: define{{.*}} void @write_through_pointers(
// CHECK: [[P:%.*]] = load ptr, ptr @p
// CHECK: store i32 1, ptr [[P]]
// CHECK: [[Q:%.*]] = load ptr addrspace(200), ptr @q
// CHECK: store i32 2, ptr addrspace(200) [[Q]]
// CHECK: [[R:%.*]] = load ptr, ptr addrspace(200) @r
// CHECK: store i32 3, ptr [[R]]
// CHECK: [[T:%.*]] = load ptr addrspace(200), ptr addrspace(200) @t
// CHECK: store i32 4, ptr addrspace(200) [[T]]
void write_through_pointers(void) {
  *p = 1;
  *q = 2;
  *r = 3;
  *t = 4;
}

// CHECK-LABEL: define{{.*}} i32 @read_external(
// CHECK: load i32, ptr addrspace(200) @protected_external
int read_external(void) { return protected_external; }

// CHECK-LABEL: define{{.*}} ptr addrspace(200) @static_storage(
// CHECK: ret ptr addrspace(200) @static_storage.value
__protected int *static_storage(void) {
  static __protected int value = 9;
  return &value;
}

// CHECK-LABEL: define{{.*}} i32 @nested_pointer(
// CHECK: [[P:%.*]] = load ptr addrspace(200), ptr %{{.*}}
// CHECK: load i32, ptr addrspace(200) [[P]]
int nested_pointer(__protected int **p) { return **p; }

// CHECK-LABEL: define{{.*}} ptr addrspace(200) @element(ptr addrspace(200)
// O0: getelementptr inbounds{{.*}} i32, ptr addrspace(200)
// OPT: getelementptr inbounds{{.*}}, ptr addrspace(200)
// CHECK: ret ptr addrspace(200)
__protected int *element(__protected int *p, int index) { return p + index; }

// CHECK-LABEL: define{{.*}} ptr addrspace(200) @null_pointer(
// CHECK: ret ptr addrspace(200) null
__protected int *null_pointer(void) { return (void *)0; }

// CHECK-LABEL: define{{.*}} ptr addrspace(200) @choose(
// O0: phi ptr addrspace(200)
// OPT: select i1 {{.*}}, ptr addrspace(200) {{.*}}, ptr addrspace(200)
// CHECK: ret ptr addrspace(200)
__protected int *choose(int condition, __protected int *p) {
  return condition ? p : (void *)0;
}

__protected void *protected_malloc(__SIZE_TYPE__ size);

// CHECK-LABEL: define{{.*}} ptr addrspace(200) @allocate(
// CHECK: call{{.*}} ptr addrspace(200) @protected_malloc(
// CHECK: ret ptr addrspace(200)
__protected int *allocate(void) { return protected_malloc(sizeof(int)); }

// The callback object is protected, while its function address remains ordinary.
// CHECK-LABEL: define{{.*}} i32 @call_callback(
// CHECK: [[FN:%.*]] = load ptr, ptr addrspace(200) @callback
// CHECK: call i32 [[FN]]()
int call_callback(void) { return callback(); }

struct record {
  int value;
  __protected int *pointer;
  int array[4];
};

// CHECK-LABEL: define{{.*}} i32 @members(ptr addrspace(200)
// O0: getelementptr inbounds{{.*}} %struct.record, ptr addrspace(200) {{.*}}, i32 0, i32 1
// OPT: getelementptr inbounds{{.*}} i8, ptr addrspace(200)
// CHECK: [[P:%.*]] = load ptr addrspace(200), ptr addrspace(200)
// CHECK: load i32, ptr addrspace(200) [[P]]
// CHECK: store i32 {{.*}}, ptr addrspace(200)
// O0: getelementptr inbounds{{.*}} %struct.record, ptr addrspace(200) {{.*}}, i32 0, i32 2
// OPT: getelementptr inbounds{{.*}}, ptr addrspace(200)
int members(__protected struct record *p, int index) {
  p->value = *p->pointer;
  return p->array[index];
}

void consume(__protected int *p);
void consume_ordinary_slot(int *__protected *p);
void consume_protected_slot(__protected int *__protected *p);

// Escaping allocations retain their storage domain even after optimization.
// CHECK-LABEL: define{{.*}} void @local_storage(
// CHECK-DAG: %scalar = alloca i32, align 4, addrspace(200)
// CHECK-DAG: %array = alloca [4 x i32], align {{[0-9]+}}, addrspace(200)
// CHECK-DAG: %ordinary_slot = alloca ptr, align {{[0-9]+}}, addrspace(200)
// CHECK-DAG: %protected_slot = alloca ptr addrspace(200), align {{[0-9]+}}, addrspace(200)
// OPT: call void @llvm.lifetime.start.p200(ptr {{.*}}addrspace(200) {{.*}}%scalar)
// CHECK: store i32 1, ptr addrspace(200) %scalar
// CHECK: store ptr {{.*}}, ptr addrspace(200) %ordinary_slot
// CHECK: store ptr addrspace(200) %scalar, ptr addrspace(200) %protected_slot
// CHECK: call void @consume(ptr addrspace(200) {{.*}}%scalar)
// CHECK: call void @consume(ptr addrspace(200)
// CHECK: call void @consume_ordinary_slot(ptr addrspace(200)
// CHECK: call void @consume_protected_slot(ptr addrspace(200)
// OPT: call void @llvm.lifetime.end.p200(ptr {{.*}}addrspace(200) {{.*}}%scalar)
void local_storage(int *p) {
  __protected int scalar = 1;
  __protected int array[4] = {1, 2, 3, 4};
  int *__protected ordinary_slot = p;
  __protected int *__protected protected_slot = &scalar;
  consume(&scalar);
  consume(array);
  consume_ordinary_slot(&ordinary_slot);
  consume_protected_slot(&protected_slot);
}

// CHECK-LABEL: define{{.*}} void @variable_array(
// CHECK: [[VLA:%.*]] = alloca i32, i{{32|64}} %{{.*}}, align {{[0-9]+}}, addrspace(200)
// CHECK: getelementptr {{.*}}, ptr addrspace(200) [[VLA]]
// CHECK: store i32 1, ptr addrspace(200)
// CHECK: call void @consume(ptr addrspace(200) {{.*}}[[VLA]])
void variable_array(int count) {
  __protected int array[count];
  array[count - 1] = 1;
  consume(array);
}

// CHECK-LABEL: define{{.*}} void @compound_literal(
// CHECK: [[OBJECT:%.*]] = alloca i32, align 4, addrspace(200)
// CHECK: store i32 3, ptr addrspace(200) [[OBJECT]]
// CHECK: call void @consume(ptr addrspace(200) {{.*}}[[OBJECT]])
void compound_literal(void) { consume(&(__protected int){3}); }

// Bulk operations carry the source and destination storage domains in IR.
// CHECK-LABEL: define{{.*}} void @zero_initialize(
// CHECK: [[ARRAY:%.*]] = alloca [16 x i32], align {{[0-9]+}}, addrspace(200)
// CHECK: call void @llvm.memset.p200.i{{32|64}}(ptr addrspace(200) {{.*}}[[ARRAY]]
// CHECK: call void @consume(ptr addrspace(200)
void zero_initialize(void) {
  __protected int array[16] = {0};
  consume(array);
}

// O0-LABEL: define{{.*}} void @copy_aggregate(
// O0: call void @llvm.memcpy.p200.p0.i{{32|64}}(ptr addrspace(200)
// O0: call void @llvm.memcpy.p0.p200.i{{32|64}}(ptr {{.*}}, ptr addrspace(200)
void copy_aggregate(__protected struct record *dst, struct record *src) {
  *dst = *src;
  *src = *dst;
}
