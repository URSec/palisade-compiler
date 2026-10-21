; RUN: opt -passes='function(palisade-local-promote),palisade-stack-split' -verify-each -S %s | FileCheck %s

target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "thumbv8.1m.main-none-eabi"

; One unsafe stack pointer per program, in protected data, starting at the
; unsafe stack top from the linker script.
; CHECK: @__palisade_unsafe_stack_ptr = weak addrspace(200) global ptr @__palisade_unsafe_stack_top, section ".data.palisade", comdat
; CHECK: @__palisade_unsafe_stack_top = external global i8

; Promoted and explicit protected locals stay on the hardware stack. Escaping
; ordinary objects move, even a slot holding a protected pointer. Check padding,
; lifetime removal, and restoration of the unsafe stack pointer on both exits.
define void @mixed(i1 %c) noinline optnone {
; CHECK-LABEL: define void @mixed(
; CHECK: [[IN:%.*]] = load volatile ptr, ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK: [[BASE:%.*]] = call ptr @llvm.ptrmask.p0.i32(ptr [[IN]], i32 -64)
; CHECK: [[TOP:%.*]] = getelementptr i8, ptr [[BASE]], i32 -64
; CHECK: store volatile ptr [[TOP]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK: [[A:%.*]] = getelementptr i8, ptr [[BASE]], i32 -3
; CHECK-NEXT: [[B:%.*]] = getelementptr i8, ptr [[BASE]], i32 -64
; CHECK-NEXT: %safe = alloca i32, align 4, addrspace(200)
; CHECK-NEXT: %protected = alloca i32, align 4, addrspace(200)
; CHECK-NEXT: store i32 1, ptr addrspace(200) %safe
; CHECK-NEXT: store ptr addrspace(200) %protected, ptr [[B]]
; CHECK-NEXT: call void @use(ptr [[A]], ptr [[B]], ptr addrspace(200) %protected)
; CHECK: left:
; CHECK-NEXT: store volatile ptr [[IN]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: ret void
; CHECK: right:
; CHECK-NEXT: store volatile ptr [[IN]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: ret void
  %a = alloca [3 x i8], align 1
  %b = alloca ptr addrspace(200), align 64
  %safe = alloca i32
  %protected = alloca i32, align 4, addrspace(200)
  call void @llvm.lifetime.start.p0(ptr %a)
  store i32 1, ptr %safe
  store ptr addrspace(200) %protected, ptr %b
  call void @use(ptr %a, ptr %b, ptr addrspace(200) %protected)
  call void @llvm.lifetime.end.p0(ptr %a)
  br i1 %c, label %left, label %right
left:
  ret void
right:
  ret void
}

; Protected-only functions keep native VLA allocation and restoration.
define void @protected_only(i32 %n) {
; CHECK-LABEL: define void @protected_only(
; CHECK-NEXT: %save = call ptr @llvm.stacksave.p0()
; CHECK-NEXT: %p = alloca i32, i32 %n, align 4, addrspace(200)
; CHECK-NEXT: call void @use(ptr null, ptr null, ptr addrspace(200) %p)
; CHECK-NEXT: call void @llvm.stackrestore.p0(ptr %save)
; CHECK-NEXT: ret void
  %save = call ptr @llvm.stacksave.p0()
  %p = alloca i32, i32 %n, addrspace(200)
  call void @use(ptr null, ptr null, ptr addrspace(200) %p)
  call void @llvm.stackrestore.p0(ptr %save)
  ret void
}

; VLA size/alignment and nested saves of both stacks. Selecting or reusing a
; stacksave result restores the matching unsafe stack pointer.
define void @vla(i32 %n, i1 %c) {
; CHECK-LABEL: define void @vla(
; CHECK: [[IN:%.*]] = load volatile ptr
; CHECK-NEXT: %saved = call ptr @llvm.stacksave.p0()
; CHECK-NEXT: [[SAVED:%.*]] = load volatile ptr
; CHECK: [[SIZE:%.*]] = mul i32 %n, 4
; CHECK: [[OLD:%.*]] = load volatile ptr
; CHECK-NEXT: [[NEG:%.*]] = sub i32 0, [[SIZE]]
; CHECK-NEXT: [[ALLOC:%.*]] = getelementptr i8, ptr [[OLD]], i32 [[NEG]]
; CHECK-NEXT: [[TOP:%.*]] = call ptr @llvm.ptrmask.p0.i32(ptr [[ALLOC]], i32 -32)
; CHECK-NEXT: store volatile ptr [[TOP]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK: %p = alloca i32, i32 %n, align 16, addrspace(200)
; CHECK: call void @use(ptr [[TOP]], ptr null, ptr addrspace(200) %p)
; CHECK: %again = call ptr @llvm.stacksave.p0()
; CHECK-NEXT: [[AGAIN:%.*]] = load volatile ptr
; CHECK: [[SELECT:%.*]] = select i1 %c, ptr [[AGAIN]], ptr [[SAVED]]
; CHECK-NEXT: %selected = select i1 %c, ptr %again, ptr %saved
; CHECK-NEXT: store volatile ptr [[SELECT]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: call void @llvm.stackrestore.p0(ptr %selected)
; CHECK-NEXT: store volatile ptr [[SAVED]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: call void @llvm.stackrestore.p0(ptr %saved)
; CHECK-NEXT: store volatile ptr [[IN]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: ret void
  %saved = call ptr @llvm.stacksave.p0()
  %a = alloca i32, i32 %n, align 32
  %p = alloca i32, i32 %n, align 16, addrspace(200)
  call void @use(ptr %a, ptr null, ptr addrspace(200) %p)
  %again = call ptr @llvm.stacksave.p0()
  %b = alloca i8, i32 %n
  call void @use(ptr %b, ptr null, ptr addrspace(200) %p)
  %selected = select i1 %c, ptr %again, ptr %saved
  call void @llvm.stackrestore.p0(ptr %selected)
  call void @llvm.stackrestore.p0(ptr %saved)
  ret void
}

; Non-entry constant allocas allocate on each iteration. A loop-carried token
; has a matching unsafe stack pointer PHI, without adding hardware-stack save
; records.
define void @loop(i32 %n) {
; CHECK-LABEL: define void @loop(
; CHECK: %saved = call ptr @llvm.stacksave.p0()
; CHECK-NEXT: [[SAVED:%.*]] = load volatile ptr
; CHECK: body:
; CHECK-NEXT: [[PHI:%.*]] = phi ptr
; CHECK-SAME: [ [[SAVED]], %entry ], [ [[PHI]], %body ]
; CHECK: [[TOP:%.*]] = load volatile ptr
; CHECK-NEXT: [[ALLOC:%.*]] = getelementptr i8, ptr [[TOP]], i32 -24
; CHECK-NEXT: [[ALIGNED:%.*]] = call ptr @llvm.ptrmask.p0.i32(ptr [[ALLOC]], i32 -8)
; CHECK-NEXT: store volatile ptr [[ALIGNED]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: call void @use(ptr [[ALIGNED]], ptr null, ptr addrspace(200) null)
; CHECK-NEXT: store volatile ptr [[PHI]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: call void @llvm.stackrestore.p0(ptr %token)
entry:
  %saved = call ptr @llvm.stacksave.p0()
  br label %body
body:
  %token = phi ptr [ %saved, %entry ], [ %token, %body ]
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %a = alloca [24 x i8], align 8
  call void @use(ptr %a, ptr null, ptr addrspace(200) null)
  call void @llvm.stackrestore.p0(ptr %token)
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}

; Move writable byval copies off the hardware stack. The unsafe stack
; pointer is restored after a tail call.
define void @byval(ptr byval([12 x i8]) align 8 %arg) {
; CHECK-LABEL: define void @byval(
; CHECK: [[IN:%.*]] = load volatile ptr
; CHECK: [[TOP:%.*]] = getelementptr i8, ptr [[IN]], i32 -16
; CHECK: store volatile ptr [[TOP]]
; CHECK: [[ARG:%.*]] = getelementptr i8, ptr [[IN]], i32 -16
; CHECK: call void @llvm.memcpy.p0.p0.i32(ptr align 8 [[ARG]], ptr align 8 %arg, i32 12, i1 false)
; CHECK: store i8 9, ptr [[ARG]]
; CHECK: call void @use(ptr [[ARG]], ptr null, ptr addrspace(200) null)
; CHECK-NEXT: tail call void @tail_target()
; CHECK-NEXT: store volatile ptr [[IN]], ptr addrspace(200) @__palisade_unsafe_stack_ptr
; CHECK-NEXT: ret void
  store i8 9, ptr %arg
  call void @use(ptr %arg, ptr null, ptr addrspace(200) null)
  tail call void @tail_target()
  ret void
}

declare void @use(ptr, ptr, ptr addrspace(200))
declare void @tail_target()
declare void @llvm.lifetime.start.p0(ptr captures(none))
declare void @llvm.lifetime.end.p0(ptr captures(none))
declare ptr @llvm.stacksave.p0()
declare void @llvm.stackrestore.p0(ptr)
