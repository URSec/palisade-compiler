// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -std=c11 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade -std=c11 -fsyntax-only -verify %s
// expected-no-diagnostics

// Pointer declarations
int *p;
__protected int *q;
int *__protected r;
__protected int *__protected t;
__protected int **nested;
int *__protected *slot_pointer;

// Multiple declarators
__protected int scalar, *pointer;
int *__protected slot, **ordinary_pointer;

// Type definitions
typedef __protected int protected_int;
typedef __protected int *protected_ptr;
typedef int *__protected protected_slot;
typedef int int_array[4];
typedef __protected int_array protected_array;
typedef int function_type(void);

__protected int global;
static __protected int internal;
extern __protected int external;
protected_array array;
const volatile __protected int cv;
int (*__protected callback)(void);
function_type *__protected callback_typedef;

// Aggregate type
struct record {
  int value;
  __protected int *pointer;
  int field;
  int *pointer_field;
};
__protected struct record aggregate;

// Function arguments and return
protected_ptr identity(__protected int *value) { return value; }
void array_parameter(__protected int values[4]);
void typedef_array_parameter(protected_array values);

// Locals, array, VLA
void locals(int count) {
  __protected int local = 1;
  protected_int via_typedef = 2;
  __protected int values[4] = {0};
  __protected int vla[count];
  __protected int *__protected pointer = &local;
  int ordinary;
  int *__protected ordinary_pointer = &ordinary;
  __protected struct record record = {0};
  static __protected int static_local;

  // Abstract declarators, casts, compound literals, and a for-loop declaration.
  (void)sizeof(__protected int *);
  (void)sizeof(int *__protected *);
  pointer = (__protected int *)pointer;
  pointer = &(__protected int){3};
  pointer = (__protected int[2]){4, 5};
  for (__protected int i = 0; i < 1; ++i)
    *pointer = i;
}
