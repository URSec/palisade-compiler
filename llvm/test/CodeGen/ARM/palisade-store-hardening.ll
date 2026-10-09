; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -arm-enable-palisade-store-hardening -verify-machineinstrs %s -o - | FileCheck %s --check-prefixes=CHECK,OPT
; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -arm-enable-palisade-store-hardening -verify-machineinstrs -O0 %s -o - | FileCheck %s --check-prefixes=CHECK,O0
; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=DISABLED
; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -arm-enable-palisade-store-hardening -verify-machineinstrs -filetype=obj %s -o /dev/null

@ordinary = global i32 0
@protected = addrspace(200) global i32 0

; DISABLED-NOT: strt
; DISABLED-NOT: strbt
; DISABLED-NOT: strht

define void @scalar(ptr %p, ptr addrspace(200) %q, i32 %x) {
; CHECK-LABEL: scalar:
; CHECK: strt
; CHECK: str{{(\.w)?}} {{.*}}[r1]
  store volatile i32 %x, ptr %p
  store volatile i32 %x, ptr addrspace(200) %q
  ret void
}

define void @widths(ptr %p, i32 %x) {
; CHECK-LABEL: widths:
; CHECK: strbt
; CHECK: strht
  %byte = trunc i32 %x to i8
  %half = trunc i32 %x to i16
  store volatile i8 %byte, ptr %p
  store volatile i16 %half, ptr %p
  ret void
}

define void @globals(i32 %x) {
; CHECK-LABEL: globals:
; CHECK: strt
; CHECK: str{{(\.w)?}}
  store volatile i32 %x, ptr @ordinary
  store volatile i32 %x, ptr addrspace(200) @protected
  ret void
}

define void @large_offset(ptr %p, i32 %x) {
; CHECK-LABEL: large_offset:
; CHECK: addw [[ADDR:r[0-9]+]], {{r[0-9]+}}, #1024
; CHECK: strt {{r[0-9]+}}, {{\[}}[[ADDR]]{{\]}}
  %element = getelementptr i8, ptr %p, i32 1024
  store volatile i32 %x, ptr %element
  ret void
}

declare ptr @llvm.stacksave.p0()

define void @save_stack_pointer(ptr %p) {
; CHECK-LABEL: save_stack_pointer:
; CHECK: mov [[VALUE:r[0-9]+]], sp
; CHECK: strt [[VALUE]],
  %sp = call ptr @llvm.stacksave.p0()
  %dst = getelementptr i8, ptr %p, i32 1024
  store ptr %sp, ptr %dst
  ret void
}

declare void @consume(ptr, ptr addrspace(200))

define void @locals(i32 %x) {
; Ordinary allocas on SP must not be confused with compiler spills.
; CHECK-LABEL: locals:
; CHECK: push
; CHECK: strt
; CHECK: str{{(\.w)?}} {{r[0-9]+}}, [sp
; CHECK: bl consume
  %a = alloca i32
  %b = alloca i32, addrspace(200)
  store volatile i32 %x, ptr %a
  store volatile i32 %x, ptr addrspace(200) %b
  call void @consume(ptr %a, ptr addrspace(200) %b)
  ret void
}

define void @wide(ptr %p, ptr addrspace(200) %q, i64 %x) {
; CHECK-LABEL: wide:
; CHECK: strt
; CHECK: strt
; OPT: strd
; O0: str {{.*}}[r1, #4]
; O0: str {{.*}}[r1]
  store i64 %x, ptr %p, align 8
  store i64 %x, ptr addrspace(200) %q, align 8
  ret void
}

define void @conditional(ptr %p, i32 %x, i1 %c) {
; CHECK-LABEL: conditional:
; CHECK: strt
  br i1 %c, label %write, label %exit
write:
  store i32 %x, ptr %p
  br label %exit
exit:
  ret void
}

define void @floating_point(ptr %p, float %x, double %y) {
; CHECK-LABEL: floating_point:
; CHECK: strt
; CHECK: strt
; CHECK: strt
  store volatile float %x, ptr %p
  store volatile double %y, ptr %p, align 8
  ret void
}

declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
declare void @llvm.memcpy.p200.p0.i32(ptr addrspace(200), ptr, i32, i1)

define void @inline_copy(ptr %p, ptr addrspace(200) %q, ptr %src) {
; CHECK-LABEL: inline_copy:
; CHECK: strt
; CHECK: strt
; CHECK: strt
; CHECK: strt
; OPT: stm
; O0: str{{(\.w)?}} {{.*}}[r1, #12]
  call void @llvm.memcpy.p0.p0.i32(ptr align 4 %p, ptr align 4 %src, i32 16, i1 false)
  call void @llvm.memcpy.p200.p0.i32(ptr addrspace(200) align 4 %q, ptr align 4 %src, i32 16, i1 false)
  ret void
}
