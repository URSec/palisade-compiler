// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -ast-dump -verify %s | FileCheck %s
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -ast-print %s > %t.c
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -ast-dump %t.c | FileCheck %s
// expected-no-diagnostics

// Check printed qualifier placement, including reparsing the AST printer's
// output. Type identity and expression propagation are tested in Sema.
int *p;
__protected int *q;
int *__protected r;
__protected int *__protected t;
// CHECK: VarDecl {{.*}} p 'int *'
// CHECK: VarDecl {{.*}} q '__protected int *'
// CHECK: VarDecl {{.*}} r 'int *__protected'
// CHECK: VarDecl {{.*}} t '__protected int *__protected'

typedef __protected int protected_int;
typedef int int_array[4];
typedef __protected int_array protected_array;
// CHECK: TypedefDecl {{.*}} protected_int '__protected int'
// CHECK: QualType {{.*}} '__protected int' __protected
// CHECK: TypedefDecl {{.*}} protected_array '__protected int_array'

const volatile __protected int cv;
int (*__protected callback)(void);
__protected int values[4];
// CHECK: VarDecl {{.*}} cv 'const volatile __protected int'
// CHECK: VarDecl {{.*}} callback 'int (*__protected)(void)'
// CHECK: VarDecl {{.*}} values '__protected int[4]'

struct record { int value; };
__protected struct record object;
// CHECK: VarDecl {{.*}} object '__protected struct record'

__protected int *identity(__protected int *value) { return value; }
// CHECK: FunctionDecl {{.*}} identity '__protected int *(__protected int *)'

void parameters(__protected int value, int *__protected slot) {}
// CHECK: ParmVarDecl {{.*}} value '__protected int'
// CHECK: ParmVarDecl {{.*}} slot 'int *__protected'

void locals(int count) {
  static __protected int static_local;
  __protected int vla[count];
  protected_int alias;
  __protected int *literal = &(__protected int){3};
}
// CHECK: VarDecl {{.*}} static_local '__protected int' static
// CHECK: VarDecl {{.*}} vla '__protected int[count]'
// CHECK: VarDecl {{.*}} alias 'protected_int':'__protected int'
// CHECK: CompoundLiteralExpr {{.*}} '__protected int' lvalue
