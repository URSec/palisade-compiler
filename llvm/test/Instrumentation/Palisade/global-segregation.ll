; RUN: opt -passes=palisade-global-segregation -S %s | FileCheck %s

target triple = "thumbv8.1m.main-none-eabi"

; Skipped: not in address space 200, or a declaration.
; CHECK: @ordinary = global i32 0{{$}}
@ordinary = global i32 0
; CHECK: @ordinary_pointer = global ptr addrspace(200) @protected_value{{$}}
@ordinary_pointer = global ptr addrspace(200) @protected_value
; CHECK: @protected_external = external addrspace(200) global i32{{$}}
@protected_external = external addrspace(200) global i32

; Read only constant goes to .rodata.palisade
; CHECK: @protected_constant = addrspace(200) constant i32 42, section ".rodata.palisade"{{$}}
@protected_constant = addrspace(200) constant i32 42
; CHECK: @protected_constant_zero = addrspace(200) constant i32 0, section ".rodata.palisade"{{$}}
@protected_constant_zero = addrspace(200) constant i32 0

; Null or undef initializer goes to .bss.palisade
; CHECK: @protected_zero = addrspace(200) global i32 0, section ".bss.palisade"{{$}}
@protected_zero = addrspace(200) global i32 0
; CHECK: @protected_undef = addrspace(200) global i32 undef, section ".bss.palisade"{{$}}
@protected_undef = addrspace(200) global i32 undef
; CHECK: @protected_padded = addrspace(200) global { i32, [4 x i8] } { i32 0, [4 x i8] undef }, section ".bss.palisade"{{$}}
@protected_padded = addrspace(200) global { i32, [4 x i8] } { i32 0, [4 x i8] undef }

; Mutable initialized variable goes to .data.palisade
; CHECK: @protected_value = addrspace(200) global i32 7, section ".data.palisade"{{$}}
@protected_value = addrspace(200) global i32 7
; CHECK: @protected_partial = addrspace(200) global { i32, [4 x i8] } { i32 1, [4 x i8] undef }, section ".data.palisade"{{$}}
@protected_partial = addrspace(200) global { i32, [4 x i8] } { i32 1, [4 x i8] undef }
