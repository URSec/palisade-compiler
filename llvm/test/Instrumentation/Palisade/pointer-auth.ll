; RUN: opt -passes=palisade-pointer-auth -verify-each -S %s | FileCheck %s

target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64-ni:200"
target triple = "thumbv8.1m.main-none-eabi"

; CHECK: @ordinary_ptr = global { ptr addrspace(200), i32 } zeroinitializer, align 4
@ordinary_ptr = global { ptr addrspace(200), i32 } zeroinitializer, align 4
; GlobalOpt shrank this global to the pointer alone, so it grows back.
; CHECK: @shrunk = internal global { ptr addrspace(200), [4 x i8] } zeroinitializer, align 4
@shrunk = internal global ptr addrspace(200) null, align 4
; Protected memory is trusted, so its initializer is not signed.
@protected_ptr = addrspace(200) global { ptr addrspace(200), i32 } { ptr addrspace(200) @value, i32 0 }, align 4
@value = addrspace(200) global i32 0, align 4

; A pointer in an initializer is signed at boot by a constructor.
; CHECK: @initialized = global { ptr addrspace(200), i32 } { ptr addrspace(200) @value, i32 0 }, align 4
@initialized = global { ptr addrspace(200), i32 } { ptr addrspace(200) @value, i32 0 }, align 4
; A constant moves out of flash to hold its PAC. Its null entry is not signed.
; CHECK: @table = global [2 x { { ptr addrspace(200), i32 }, i32 }]
@table = constant [2 x { { ptr addrspace(200), i32 }, i32 }] [{ { ptr addrspace(200), i32 }, i32 } { { ptr addrspace(200), i32 } zeroinitializer, i32 1 }, { { ptr addrspace(200), i32 }, i32 } { { ptr addrspace(200), i32 } { ptr addrspace(200) @value, i32 0 }, i32 2 }], align 4
; CHECK: @shrunk_initialized = internal global { ptr addrspace(200), [4 x i8] } { ptr addrspace(200) @value, [4 x i8] zeroinitializer }, align 4
@shrunk_initialized = internal global ptr addrspace(200) @value, align 4
; CHECK: @llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 0, ptr @palisade.sign.globals, ptr null }]

; A store signs the pointer with its address, and stores the PAC after it.
define void @store(ptr addrspace(200) %p) {
; CHECK-LABEL: define void @store(
; CHECK-NEXT: %palisade.pac = call i32 @llvm.arm.protected.sign(ptr addrspace(200) %p, i32 ptrtoint (ptr @ordinary_ptr to i32))
; CHECK-NEXT: store ptr addrspace(200) %p, ptr @ordinary_ptr, align 4
; CHECK-NEXT: store i32 %palisade.pac, ptr getelementptr inbounds (i8, ptr @ordinary_ptr, i32 4), align 4
; CHECK-NEXT: ret void
  store ptr addrspace(200) %p, ptr @ordinary_ptr, align 4
  ret void
}

; A load checks the pointer against the PAC after it, and its uses take the
; checked pointer.
define i32 @load(ptr %s) {
; CHECK-LABEL: define i32 @load(
; CHECK-NEXT: %field = getelementptr inbounds i8, ptr %s, i32 4
; CHECK-NEXT: %p = load ptr addrspace(200), ptr %field, align 4
; CHECK-NEXT: %palisade.pac.addr = getelementptr inbounds i8, ptr %field, i32 4
; CHECK-NEXT: %palisade.pac = load i32, ptr %palisade.pac.addr, align 4
; CHECK-NEXT: %palisade.modifier = ptrtoint ptr %field to i32
; CHECK-NEXT: %palisade.auth = call ptr addrspace(200) @llvm.arm.protected.auth(ptr addrspace(200) %p, i32 %palisade.pac, i32 %palisade.modifier)
; CHECK-NEXT: %x = load i32, ptr addrspace(200) %palisade.auth, align 4
; CHECK-NEXT: ret i32 %x
  %field = getelementptr inbounds i8, ptr %s, i32 4
  %p = load ptr addrspace(200), ptr %field, align 4
  %x = load i32, ptr addrspace(200) %p, align 4
  ret i32 %x
}

; A copy checks at the source and signs again for the destination. The PAC
; accesses stay volatile.
define void @copy_volatile(ptr %dst, ptr %src) {
; CHECK-LABEL: define void @copy_volatile(
; CHECK-NEXT: %p = load volatile ptr addrspace(200), ptr %src, align 4
; CHECK-NEXT: %palisade.pac.addr1 = getelementptr inbounds i8, ptr %src, i32 4
; CHECK-NEXT: %palisade.pac2 = load volatile i32, ptr %palisade.pac.addr1, align 4
; CHECK-NEXT: %palisade.modifier3 = ptrtoint ptr %src to i32
; CHECK-NEXT: %palisade.auth = call ptr addrspace(200) @llvm.arm.protected.auth(ptr addrspace(200) %p, i32 %palisade.pac2, i32 %palisade.modifier3)
; CHECK-NEXT: %palisade.modifier = ptrtoint ptr %dst to i32
; CHECK-NEXT: %palisade.pac = call i32 @llvm.arm.protected.sign(ptr addrspace(200) %palisade.auth, i32 %palisade.modifier)
; CHECK-NEXT: store volatile ptr addrspace(200) %palisade.auth, ptr %dst, align 4
; CHECK-NEXT: %palisade.pac.addr = getelementptr inbounds i8, ptr %dst, i32 4
; CHECK-NEXT: store volatile i32 %palisade.pac, ptr %palisade.pac.addr, align 4
; CHECK-NEXT: ret void
  %p = load volatile ptr addrspace(200), ptr %src, align 4
  store volatile ptr addrspace(200) %p, ptr %dst, align 4
  ret void
}

; Loading null skips the check, so storing null needs no PAC.
define void @store_null() {
; CHECK-LABEL: define void @store_null(
; CHECK-NEXT: store ptr addrspace(200) null, ptr @ordinary_ptr, align 4
; CHECK-NEXT: ret void
  store ptr addrspace(200) null, ptr @ordinary_ptr, align 4
  ret void
}

; Protected memory is trusted.
define i32 @protected(ptr addrspace(200) %p) {
; CHECK-LABEL: define i32 @protected(
; CHECK-NEXT: store ptr addrspace(200) %p, ptr addrspace(200) @protected_ptr, align 4
; CHECK-NEXT: %q = load ptr addrspace(200), ptr addrspace(200) @protected_ptr, align 4
; CHECK-NEXT: %x = load i32, ptr addrspace(200) %q, align 4
; CHECK-NEXT: ret i32 %x
  store ptr addrspace(200) %p, ptr addrspace(200) @protected_ptr, align 4
  %q = load ptr addrspace(200), ptr addrspace(200) @protected_ptr, align 4
  %x = load i32, ptr addrspace(200) %q, align 4
  ret i32 %x
}

; A local shrunk by SROA grows back like @shrunk.
define void @grow(ptr addrspace(200) %p) {
; CHECK-LABEL: define void @grow(
; CHECK-NEXT: %local = alloca { ptr addrspace(200), [4 x i8] }, align 4
; CHECK-NEXT: %palisade.modifier = ptrtoint ptr %local to i32
; CHECK-NEXT: %palisade.pac = call i32 @llvm.arm.protected.sign(ptr addrspace(200) %p, i32 %palisade.modifier)
; CHECK-NEXT: store ptr addrspace(200) %p, ptr %local, align 4
; CHECK-NEXT: %palisade.pac.addr = getelementptr inbounds i8, ptr %local, i32 4
; CHECK-NEXT: store i32 %palisade.pac, ptr %palisade.pac.addr, align 4
; CHECK-NEXT: call void @use(ptr %local)
; CHECK-NEXT: %palisade.pac1 = call i32 @llvm.arm.protected.sign(ptr addrspace(200) %p, i32 ptrtoint (ptr @shrunk to i32))
; CHECK-NEXT: store ptr addrspace(200) %p, ptr @shrunk, align 4
; CHECK-NEXT: store i32 %palisade.pac1, ptr getelementptr inbounds (i8, ptr @shrunk, i32 4), align 4
; CHECK-NEXT: ret void
  %local = alloca ptr addrspace(200), align 4
  store ptr addrspace(200) %p, ptr %local, align 4
  call void @use(ptr %local)
  store ptr addrspace(200) %p, ptr @shrunk, align 4
  ret void
}

declare void @use(ptr)

; The constructor signs the pointers in memory, after the linker has picked
; each global's definition.
; CHECK-LABEL: define internal void @palisade.sign.globals(
; CHECK-NEXT: [[P:%.*]] = load ptr addrspace(200), ptr @initialized, align 4
; CHECK-NEXT: %palisade.pac = call i32 @llvm.arm.protected.sign(ptr addrspace(200) [[P]], i32 ptrtoint (ptr @initialized to i32))
; CHECK-NEXT: store i32 %palisade.pac, ptr getelementptr inbounds (i8, ptr @initialized, i32 4), align 4
; CHECK-NEXT: [[P:%.*]] = load ptr addrspace(200), ptr getelementptr inbounds (i8, ptr @table, i32 12), align 4
; CHECK-NEXT: %palisade.pac1 = call i32 @llvm.arm.protected.sign(ptr addrspace(200) [[P]], i32 ptrtoint (ptr getelementptr inbounds (i8, ptr @table, i32 12) to i32))
; CHECK-NEXT: store i32 %palisade.pac1, ptr getelementptr inbounds (i8, ptr getelementptr inbounds (i8, ptr @table, i32 12), i32 4), align 4
; CHECK-NEXT: [[P:%.*]] = load ptr addrspace(200), ptr @shrunk_initialized, align 4
; CHECK-NEXT: %palisade.pac2 = call i32 @llvm.arm.protected.sign(ptr addrspace(200) [[P]], i32 ptrtoint (ptr @shrunk_initialized to i32))
; CHECK-NEXT: store i32 %palisade.pac2, ptr getelementptr inbounds (i8, ptr @shrunk_initialized, i32 4), align 4
; CHECK-NEXT: ret void
