// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade-pac -std=c11 -fsyntax-only -ferror-limit 0 -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fpalisade-pac -std=c11 -fsyntax-only -ferror-limit 0 -verify %s
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade-pac -std=c23 -fsyntax-only -ferror-limit 0 -verify %s

// Type identity, propagation and the existing address-space diagnostics.
// Palisade's rules for casts, function pointers, comparisons and return types.
#define SAME(A, B) __builtin_types_compatible_p(A, B)
#define CHECK_TYPE(E, T) _Static_assert(SAME(__typeof__(E), T), #E)

typedef __protected int protected_int;
typedef __protected int *protected_ptr;
typedef int *__protected protected_slot;
typedef __protected int protected_array[4];

// Compare addresses for object storage.
int *p;
__protected int *q;
int *__protected r;
__protected int *__protected t;
_Static_assert(!SAME(__typeof__(p), __typeof__(q)), "pointee domain");
_Static_assert(!SAME(__typeof__(&p), __typeof__(&r)), "pointer storage domain");
CHECK_TYPE(&q, __protected int **);
CHECK_TYPE(&r, int *__protected *);
CHECK_TYPE(&t, __protected int *__protected *);
_Static_assert(SAME(protected_slot *, int *__protected *), "slot typedef");
_Static_assert(SAME(protected_array *, __protected int (*)[4]), "array typedef");
_Static_assert(!SAME(protected_int *, int __attribute__((address_space(200))) *),
               "language domain differs from a numeric target space");
_Static_assert(sizeof(protected_int) == sizeof(int), "object size");
_Static_assert(_Alignof(protected_int) == _Alignof(int), "object alignment");
// A pointer to protected storage is followed by a PAC word wherever it is
// stored, protected slots included. Its alignment and value width don't change.
_Static_assert(sizeof(q) == 2 * sizeof(p), "pointer size");
_Static_assert(sizeof(t) == sizeof(q), "protected slot size");
_Static_assert(sizeof(r) == sizeof(p), "ordinary pointer size");
_Static_assert(sizeof(protected_ptr *) == sizeof(int *), "pointer to a slot");
_Static_assert(_Alignof(protected_ptr) == _Alignof(int *), "pointer alignment");
__UINTPTR_TYPE__ protected_address = (__UINTPTR_TYPE__)&t;
__UINTPTR_TYPE__ pointer_value(__protected int *p) {
  return (__UINTPTR_TYPE__)p;
}

// Members inherit storage from their containing object recursively.
struct record {
  int value;
  protected_ptr pointer;
  int *ordinary_pointer;
  int array[4];
  struct { int value; } nested;
};
struct record ordinary_record;
__protected struct record protected_record;
const volatile __protected struct record qualified_record;
__protected struct record *record_pointer;
CHECK_TYPE(&ordinary_record.value, int *);
CHECK_TYPE(&ordinary_record.pointer, __protected int **);
CHECK_TYPE(&protected_record.value, __protected int *);
CHECK_TYPE(&protected_record.pointer, __protected int *__protected *);
CHECK_TYPE(&protected_record.ordinary_pointer, int *__protected *);
CHECK_TYPE(&protected_record.array[0], __protected int *);
CHECK_TYPE(&protected_record.nested.value, __protected int *);
CHECK_TYPE(&qualified_record.value, const volatile __protected int *);
CHECK_TYPE(&record_pointer->value, __protected int *);

// Local and parameter storage, array adjustment, and expression propagation.
void propagation(int count, __protected int array[4], __protected int value,
                 int *__protected slot, __protected int **pp) {
  protected_int local;
  __protected int vla[count];
  __protected int matrix[2][3];
  CHECK_TYPE(array, __protected int *);
  CHECK_TYPE(&value, __protected int *);
  CHECK_TYPE(&slot, int *__protected *);
  CHECK_TYPE(&local, __protected int *);
  CHECK_TYPE(&vla[0], __protected int *);
  CHECK_TYPE(&matrix[0], __protected int (*)[3]);
  CHECK_TYPE(&matrix[0][0], __protected int *);
  CHECK_TYPE(*pp, __protected int *);
  CHECK_TYPE(&**pp, __protected int *);
  CHECK_TYPE(&(__protected int){3}, __protected int *);
  __protected int *decayed = vla;
  __protected int (*row)[3] = matrix;
  int ordinary_array[4];
  int *bad_decay = vla; // expected-error {{changes address space of pointer}}
  __protected int *bad_ordinary_decay = ordinary_array; // expected-error {{changes address space of pointer}}

  // Loading a scalar does not attach its storage domain to the value.
  int scalar = *decayed;
  *decayed = scalar;
}

// Redeclarations: matching typedefs, then conflicts at each qualifier level.
extern protected_int agreed_object;
__protected int agreed_object;
extern protected_ptr agreed_pointer;
__protected int *agreed_pointer;
extern protected_slot agreed_slot;
int *__protected agreed_slot;
extern int conflicting_object; // expected-note {{previous declaration is here}}
extern __protected int conflicting_object; // expected-error {{redeclaration of 'conflicting_object' with a different type}}
extern __protected int *conflicting_pointer; // expected-note {{previous declaration is here}}
extern int *conflicting_pointer; // expected-error {{redeclaration of 'conflicting_pointer' with a different type}}
extern int *conflicting_slot; // expected-note {{previous declaration is here}}
extern int *__protected conflicting_slot; // expected-error {{redeclaration of 'conflicting_slot' with a different type}}
void conflicting_parameter(int *); // expected-note {{previous declaration is here}}
void conflicting_parameter(__protected int *); // expected-error {{conflicting types for 'conflicting_parameter'}}
int *conflicting_return(void); // expected-note {{previous declaration is here}}
__protected int *conflicting_return(void); // expected-error {{conflicting types for 'conflicting_return'}}

void take_ordinary(int *p); // expected-note {{passing argument to parameter 'p' here}}
void take_protected(__protected int *p); // expected-note {{passing argument to parameter 'p' here}}

void direct_conversions(int *ordinary, __protected int *protected) {
  __protected int *copy = protected;
  copy = protected;
  take_protected(copy);
  int *bad_ordinary = protected; // expected-error {{changes address space of pointer}}
  __protected int *bad_protected = ordinary; // expected-error {{changes address space of pointer}}
  ordinary = protected; // expected-error {{changes address space of pointer}}
  protected = ordinary; // expected-error {{changes address space of pointer}}
  take_ordinary(protected); // expected-error {{changes address space of pointer}}
  take_protected(ordinary); // expected-error {{changes address space of pointer}}
}

__protected int *return_protected(__protected int *p) { return p; }
int *return_wrong_ordinary(__protected int *p) {
  return p; // expected-error {{changes address space of pointer}}
}
__protected int *return_wrong_protected(int *p) {
  return p; // expected-error {{changes address space of pointer}}
}
// A return value has no storage to protect.
__protected int qualified_return(void); // expected-error {{return type cannot be qualified with address space}}
int *__protected qualified_pointer_return(void); // expected-error {{return type cannot be qualified with address space}}

// Distinguish immediate slot mismatches from nested pointee mismatches.
// A third indirection checks that the nested comparison continues recursively.
void nested_conversions(int **ordinary, __protected int **pointee,
                        int *__protected *slot,
                        int ***ordinary3, __protected int ***protected3) {
  __protected int **copy = pointee;
  __protected int **bad = ordinary; // expected-error {{changes address space of nested pointer}}
  ordinary = pointee; // expected-error {{changes address space of nested pointer}}
  slot = ordinary; // expected-error {{changes address space of pointer}}
  ordinary = slot; // expected-error {{changes address space of pointer}}
  ordinary3 = protected3; // expected-error {{changes address space of nested pointer}}
}

// void conversions preserve the immediate pointee domain.
void void_conversions(__protected int *p, __protected void *v, void *ordinary) {
  v = p;
  p = v;
  void *bad_ordinary = p; // expected-error {{changes address space of pointer}}
  p = ordinary; // expected-error {{changes address space of pointer}}
  v = ordinary; // expected-error {{changes address space of pointer}}
}

// Casts can't make a pointer to protected storage from an ordinary pointer or
// an integer, at any level. Casting out of protected storage and casting a
// null pointer constant stay valid.
void casts(int *ordinary, __protected int *p, int **ordinary_slot,
           int *__protected *slot, __UINTPTR_TYPE__ address) {
  (void)(int *)p;
  (void)(int **)&p;
  (void)(__protected void *)p;
  (void)(__protected int *)0;
  (void)(__protected int *)(void *)0;
  (void)(__protected int *)ordinary; // expected-error {{casting 'int *' to type '__protected int *' changes address space of pointer}}
  (void)(__protected int **)ordinary_slot; // expected-error {{changes address space of nested pointer}}
  (void)(__protected int *__protected *)slot; // expected-error {{changes address space of nested pointer}}
  (void)(__protected int *)address; // expected-error {{cannot cast integer type}}
  (void)(__protected int *)0x20001000; // expected-error {{cannot cast integer type 'int' to pointer to protected storage '__protected int *'}}
}

void pointer_operators(int condition, int *ordinary,
                      __protected int *p, __protected int *q,
                      __protected void *v) {
  CHECK_TYPE(condition ? p : q, __protected int *);
  CHECK_TYPE(condition ? p : v, __protected void *);
  (void)(condition ? ordinary : p); // expected-error {{pointers to non-overlapping address spaces}}
  (void)(condition ? p : ordinary); // expected-error {{pointers to non-overlapping address spaces}}
  (void)(p - q);
  (void)(ordinary - p); // expected-error {{pointers to non-overlapping address spaces}}
  (void)(p == v);
  (void)(p == ordinary); // expected-error {{('__protected int *' and 'int *') which are pointers to non-overlapping address spaces}}
  (void)(ordinary < p); // expected-error {{pointers to non-overlapping address spaces}}
}

// Function-pointer storage is qualifiable; function types themselves are not.
typedef int function_type(void);
function_type *__protected callback;
CHECK_TYPE(&callback, function_type *__protected *);
__protected function_type invalid_function; // expected-error {{function type may not be qualified with an address space}}

// A function with pointers to protected storage in its signature only converts
// to the same function type, or a call could pass it ordinary pointers.
void store_protected(__protected int *);
void store_ordinary(int *);
void register_protected(void (*)(__protected int *));
typedef void (*ordinary_function)(int *);
typedef void (*protected_function)(__protected int *);
typedef void (*unprototyped_function)();
void function_pointers(void *v) {
  protected_function same = (protected_function)store_protected;
  ordinary_function unrelated = (ordinary_function)(void *)store_ordinary;
  protected_function null = (protected_function)0;
  (void)(ordinary_function)store_protected; // expected-error {{conversion from 'void (*)(__protected int *)' to 'ordinary_function' (aka 'void (*)(int *)') changes the signature of a function with pointers to protected storage}}
  (void)(protected_function)store_ordinary; // expected-error {{changes the signature of a function with pointers to protected storage}}
  (void)(unprototyped_function)store_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  (void)(void (*)(ordinary_function))register_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  (void)(void *)store_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  (void)(__UINTPTR_TYPE__)store_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  (void)(protected_function)v; // expected-error {{changes the signature of a function with pointers to protected storage}}
  ordinary_function implicit = store_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  unprototyped_function implicit_unprototyped = store_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  void *implicit_void = store_protected; // expected-error {{changes the signature of a function with pointers to protected storage}}
  protected_function implicit_from_void = v; // expected-error {{changes the signature of a function with pointers to protected storage}}
}

// Duplicate and conflicting qualifiers, including qualifiers hidden by aliases.
// Both attribute orders matter: they use different attribute handlers.
__protected __protected int duplicate; // expected-warning {{multiple identical address spaces specified for type}}
__protected protected_int duplicate_alias; // expected-warning {{multiple identical address spaces specified for type}}
                                           //
__protected int __attribute__((address_space(1))) *conflicting_pointee; // expected-error {{multiple address spaces specified for type}}
int __attribute__((address_space(1))) __protected *reversed_conflict; // expected-error {{multiple address spaces specified for type}}
typedef int __attribute__((address_space(1))) other_domain_int;
__protected other_domain_int *conflicting_alias; // expected-error {{multiple address spaces specified for type}}

// Null constants: static/local initialization, assignment, calls, return and
// conditional result types. (void *)0 must not erase the protected domain.
__protected int *null_global = 0;
void null_pointers(int condition, __protected int *p) {
  __protected int *null_local = (void *)0;
  p = 0;
  take_protected((void *)0);
  CHECK_TYPE(condition ? 0 : p, __protected int *);
  CHECK_TYPE(condition ? p : (void *)0, __protected int *);
  (void)(p == 0);
}

__protected int *return_null(void) { return 0; }

// Fields cannot declare independent storage domains. Cover scalar, pointer,
// typedef, array and union cases; valid pointer fields are in struct record.
struct Mixed {
  int ordinary;
  __protected int critical; // expected-error {{field may not be qualified with an address space}}
};
struct InvalidFields {
  int *__protected slot; // expected-error {{field may not be qualified with an address space}}
  protected_int scalar_alias; // expected-error {{field may not be qualified with an address space}}
  protected_array array_alias; // expected-error {{field may not be qualified with an address space}}
};
union InvalidUnion {
  int ordinary;
  __protected int critical; // expected-error {{field may not be qualified with an address space}}
};
