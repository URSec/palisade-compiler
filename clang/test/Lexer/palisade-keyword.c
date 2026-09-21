// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fpalisade -DPALISADE -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple thumbv8.1m.main-none-eabi -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c++ -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple spir-unknown-unknown -x cl -cl-std=CL2.0 -fsyntax-only -verify %s
// expected-no-diagnostics

#ifdef PALISADE
#if __is_identifier(__protected)
#error __protected must be a keyword in Palisade C
#endif
__protected int object;

// Sharing the keyword-attribute parser does not enable OpenCL keywords.
#if !__is_identifier(__generic) || !__is_identifier(generic) || \
    !__is_identifier(__global) || !__is_identifier(__local) || \
    !__is_identifier(__constant) || !__is_identifier(__private)
#error Palisade must not enable OpenCL address-space keywords
#endif
int __generic, generic, __global, __local, __constant, __private;
#else
#if !__is_identifier(__protected)
#error __protected must remain an identifier without Palisade
#endif
int __protected;
int use_identifier(void) { return __protected; }
#endif

// Palisade introduces no unprefixed spelling.
#if !__is_identifier(protected) && !defined(__cplusplus)
#error protected must remain an identifier in C
#endif
