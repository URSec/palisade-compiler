; REQUIRES: arm-registered-target
; RUN: opt -mtriple=thumbv8.1m.main-none-eabi -passes=palisade-inline-copy -S %s | FileCheck %s

target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64-ni:200"

@protected_block = addrspace(200) global [64 x i32] zeroinitializer, align 4

; A copy into ordinary memory may call the C library.
define void @ordinary(ptr %p) {
; CHECK-LABEL: @ordinary(
; CHECK: call void @llvm.memset.p0.i32(ptr align 4 %p, i8 0, i32 256, i1 false)
  call void @llvm.memset.p0.i32(ptr align 4 %p, i8 0, i32 256, i1 false)
  ret void
}

; Up to the inline limit, the copy becomes its .inline form.
define void @small(ptr addrspace(200) %d, ptr %s) {
; CHECK-LABEL: @small(
; CHECK: call void @llvm.memcpy.inline.p200.p0.i32(ptr addrspace(200) align 4 %d, ptr align 4 %s, i32 64, i1 false)
; CHECK: call void @llvm.memset.inline.p200.i32(ptr addrspace(200) align 4 %d, i8 0, i32 64, i1 false)
  call void @llvm.memcpy.p200.p0.i32(ptr addrspace(200) align 4 %d, ptr align 4 %s, i32 64, i1 false)
  call void @llvm.memset.p200.i32(ptr addrspace(200) align 4 %d, i8 0, i32 64, i1 false)
  ret void
}

; A larger copy becomes a loop that keeps the protected address space. The
; local is realigned so the loop sets two words at a time.
define void @large_local() {
; CHECK-LABEL: @large_local(
; CHECK:     %buf = alloca [256 x i8], align 4, addrspace(200)
; CHECK-NOT: call void @llvm.memset
; CHECK:     store i64 0, ptr addrspace(200) %{{.*}}, align 4
  %buf = alloca [256 x i8], align 1, addrspace(200)
  call void @llvm.memset.p200.i32(ptr addrspace(200) align 1 %buf, i8 0, i32 256, i1 false)
  call void @use(ptr addrspace(200) %buf)
  ret void
}

; Copying from ordinary memory reads ordinary memory and writes protected.
define void @large_copy(ptr align 4 %s) {
; CHECK-LABEL: @large_copy(
; CHECK-NOT: call void @llvm.memcpy
; CHECK:     load i64, ptr %{{.*}}, align 4
; CHECK:     store i64 %{{.*}}, ptr addrspace(200) %{{.*}}, align 4
  call void @llvm.memcpy.p200.p0.i32(ptr addrspace(200) align 4 @protected_block, ptr align 4 %s, i32 256, i1 false)
  ret void
}

; A size known only at run time and memmove also become loops.
define void @unknown_size(ptr addrspace(200) align 4 %d, i32 %n) {
; CHECK-LABEL: @unknown_size(
; CHECK-NOT: call void @llvm.mem
; CHECK:     store i64 0, ptr addrspace(200)
; CHECK:     store i8 0, ptr addrspace(200)
; CHECK-NOT: call void @llvm.mem
; CHECK:     ret void
  call void @llvm.memset.p200.i32(ptr addrspace(200) align 4 %d, i8 0, i32 %n, i1 false)
  ret void
}

define void @move(ptr addrspace(200) %d, ptr addrspace(200) %s) {
; CHECK-LABEL: @move(
; CHECK-NOT: call void @llvm.memmove
; CHECK:     store i8 %{{.*}}, ptr addrspace(200)
; CHECK:     ret void
  call void @llvm.memmove.p200.p200.i32(ptr addrspace(200) %d, ptr addrspace(200) %s, i32 16, i1 false)
  ret void
}

declare void @use(ptr addrspace(200))
