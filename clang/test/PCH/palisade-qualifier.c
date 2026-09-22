// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -include %s -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -emit-pch -o %t %s
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -include-pch %t -ast-dump-all -verify %s | FileCheck %s
// expected-no-diagnostics

// Run the same consumer after textual inclusion and PCH deserialization.
// The imported declarations must retain storage and pointee qualifications.
#ifndef PALISADE_TYPES
#define PALISADE_TYPES
typedef __protected int protected_int;
typedef protected_int protected_array[4];
__protected int *q;
int *__protected r;
__protected int *__protected t;
protected_array values;
struct record {
  int field;
  __protected int *pointer;
};
__protected struct record object;
// CHECK: TypedefDecl {{.*}} imported referenced protected_int '__protected int'
// CHECK: QualType {{.*}} '__protected int' __protected
// CHECK: VarDecl {{.*}} imported used q '__protected int *'
// CHECK: VarDecl {{.*}} imported used r 'int *__protected'
// CHECK: VarDecl {{.*}} imported used t '__protected int *__protected'
// CHECK: VarDecl {{.*}} imported used values 'protected_array':'protected_int[4]'
// CHECK: FieldDecl {{.*}} imported referenced field 'int'
// CHECK: FieldDecl {{.*}} imported referenced pointer '__protected int *'
#else
void use_imported(void) {
  __protected int *pointee = q;
  int *__protected *slot = &r;
  __protected int *__protected *both = &t;
  __protected int (*array)[4] = &values;
  __protected int *field = &object.field;
  __protected int *__protected *pointer_field = &object.pointer;
}
#endif
