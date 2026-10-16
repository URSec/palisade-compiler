; RUN: opt -passes=palisade-local-promote -S %s | FileCheck %s

target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"

; A write with a provably bounded index is protected, even under optnone.
define void @bounded_write(i32 %n) noinline optnone {
; CHECK-LABEL: @bounded_write(
; CHECK: %a = alloca [8 x i32], align 4, addrspace(200)
; CHECK: %p = getelementptr inbounds [8 x i32], ptr addrspace(200) %a, i32 0, i32 %i
; CHECK: store i32 1, ptr addrspace(200) %p
  %a = alloca [8 x i32], align 4
  %i = and i32 %n, 7
  %p = getelementptr inbounds [8 x i32], ptr %a, i32 0, i32 %i
  store i32 1, ptr %p
  ret void
}

; An unbounded write keeps the object ordinary; an unbounded read does not.
define i32 @unbounded(i32 %n) {
; CHECK-LABEL: @unbounded(
; CHECK: %w = alloca [8 x i32], align 4{{$}}
; CHECK: %r = alloca [8 x i32], align 4, addrspace(200)
; CHECK: %v = load i32, ptr addrspace(200) %q
  %w = alloca [8 x i32], align 4
  %r = alloca [8 x i32], align 4
  %p = getelementptr [8 x i32], ptr %w, i32 0, i32 %n
  store i32 1, ptr %p
  store i32 1, ptr %r
  %q = getelementptr [8 x i32], ptr %r, i32 0, i32 %n
  %v = load i32, ptr %q
  ret i32 %v
}

; Storing the address or converting it to an integer escapes the object.
define i32 @escapes(ptr %out) {
; CHECK-LABEL: @escapes(
; CHECK: %stored = alloca i32, align 4{{$}}
; CHECK: %int = alloca i32, align 4{{$}}
  %stored = alloca i32, align 4
  %int = alloca i32, align 4
  store ptr %stored, ptr %out
  %r = ptrtoint ptr %int to i32
  ret i32 %r
}

; A callee may read through the address, but not keep it, write through it,
; or receive an ABI copy. Calls keep their signatures through a cast.
define void @calls() {
; CHECK-LABEL: @calls(
; CHECK: %none = alloca i32, align 4, addrspace(200)
; CHECK: %read = alloca i32, align 4, addrspace(200)
; CHECK: %capture = alloca i32, align 4{{$}}
; CHECK: %write = alloca i32, align 4{{$}}
; CHECK: %byval = alloca i32, align 4{{$}}
; CHECK: call void @no_access(ptr %none.generic)
; CHECK: call void @reader(ptr %read.generic)
  %none = alloca i32, align 4
  %read = alloca i32, align 4
  %capture = alloca i32, align 4
  %write = alloca i32, align 4
  %byval = alloca i32, align 4
  call void @no_access(ptr %none)
  call void @reader(ptr %read)
  call void @capture(ptr %capture)
  call void @writer(ptr %write)
  call void @byval_callee(ptr byval(i32) %byval)
  ret void
}

; Memory intrinsics need a constant in-bounds length on the written side only.
define void @memory(ptr %ext, i32 %n) {
; CHECK-LABEL: @memory(
; CHECK: %a = alloca [16 x i8], align 4, addrspace(200)
; CHECK: %src = alloca [16 x i8], align 4, addrspace(200)
; CHECK: %oob = alloca [16 x i8], align 4{{$}}
; CHECK: %dyn = alloca [16 x i8], align 4{{$}}
; CHECK: call void @llvm.lifetime.start.p200(ptr addrspace(200) %a)
; CHECK: call void @llvm.memset.p200.i32(ptr addrspace(200) align 4 %a, i8 0, i32 16, i1 false)
; CHECK: call void @llvm.memcpy.p200.p200.i32(ptr addrspace(200) align 4 %a, ptr addrspace(200) align 4 %src, i32 16, i1 false)
; CHECK: call void @llvm.memcpy.p0.p200.i32(ptr %ext, ptr addrspace(200) %src, i32 %n, i1 false)
  %a = alloca [16 x i8], align 4
  %src = alloca [16 x i8], align 4
  %oob = alloca [16 x i8], align 4
  %dyn = alloca [16 x i8], align 4
  call void @llvm.lifetime.start.p0(ptr %a)
  call void @llvm.memset.p0.i32(ptr align 4 %a, i8 0, i32 16, i1 false)
  call void @llvm.memcpy.p0.p0.i32(ptr align 4 %a, ptr align 4 %src, i32 16, i1 false)
  call void @llvm.memcpy.p0.p0.i32(ptr %ext, ptr %src, i32 %n, i1 false)
  call void @llvm.memset.p0.i32(ptr %oob, i8 0, i32 17, i1 false)
  call void @llvm.memset.p0.i32(ptr %dyn, i8 0, i32 %n, i1 false)
  ret void
}

; A pointer that may point to another object keeps the local ordinary.
define i32 @merge(i1 %c, ptr %ext) {
; CHECK-LABEL: @merge(
; CHECK: %a = alloca i32, align 4{{$}}
; CHECK: %p = select i1 %c, ptr %a, ptr %ext
  %a = alloca i32, align 4
  %p = select i1 %c, ptr %a, ptr %ext
  %v = load i32, ptr %p
  ret i32 %v
}

; Protected locals stay as they are; variable-size locals stay ordinary.
define void @sizes(i32 %n) {
; CHECK-LABEL: @sizes(
; CHECK: %protected = alloca i32, align 4, addrspace(200)
; CHECK: %vla = alloca i32, i32 %n, align 4{{$}}
  %protected = alloca i32, align 4, addrspace(200)
  %vla = alloca i32, i32 %n, align 4
  store i32 1, ptr addrspace(200) %protected
  store i32 1, ptr %vla
  ret void
}

declare void @no_access(ptr captures(none)) memory(none)
declare void @reader(ptr captures(none) readonly)
declare void @capture(ptr) memory(read)
declare void @writer(ptr captures(none))
declare void @byval_callee(ptr byval(i32) captures(none)) memory(none)
declare void @llvm.lifetime.start.p0(ptr captures(none))
declare void @llvm.memset.p0.i32(ptr captures(none), i8, i32, i1 immarg)
declare void @llvm.memcpy.p0.p0.i32(ptr captures(none), ptr captures(none), i32, i1 immarg)
