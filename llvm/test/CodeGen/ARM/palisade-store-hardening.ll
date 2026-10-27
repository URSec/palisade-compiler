; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -arm-enable-palisade-store-hardening -verify-machineinstrs %s -o - | FileCheck %s
; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=DISABLED

; DISABLED-NOT: {{strt|strbt|strht}}

; Ordinary stores become unprivileged; protected stores stay privileged.
define void @domains(ptr %p, ptr addrspace(200) %q, i32 %x) {
; CHECK-LABEL: domains:
; CHECK:       strt r2, [r0]
; CHECK-NEXT:  str r2, [r1]
  store volatile i32 %x, ptr %p
  store volatile i32 %x, ptr addrspace(200) %q
  ret void
}

define void @widths(ptr %p, i32 %x) {
; CHECK-LABEL: widths:
; CHECK:       strbt r1, [r0]
; CHECK-NEXT:  strht r1, [r0]
  %byte = trunc i32 %x to i8
  %half = trunc i32 %x to i16
  store volatile i8 %byte, ptr %p
  store volatile i16 %half, ptr %p
  ret void
}

; Allocas share SP, so the domain comes from the alloca, not the base register.
define void @locals(i32 %x) {
; CHECK-LABEL: locals:
; CHECK:       strt r0, [sp, #4]
; CHECK-NEXT:  str r0, [sp]
  %a = alloca i32
  %b = alloca i32, addrspace(200)
  store volatile i32 %x, ptr %a
  store volatile i32 %x, ptr addrspace(200) %b
  call void @consume(ptr %a, ptr addrspace(200) %b)
  ret void
}

declare void @consume(ptr, ptr addrspace(200))

; Hardening runs before if-conversion, which predicates the STRT.
define void @conditional(ptr %p, i32 %x, i1 %c) {
; CHECK-LABEL: conditional:
; CHECK:       it ne
; CHECK-NEXT:  strtne r1, [r0]
  br i1 %c, label %write, label %exit
write:
  store i32 %x, ptr %p
  br label %exit
exit:
  ret void
}

%Registers = type { i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }
declare void @use(ptr byval([4096 x i8]))

; Every GPR is live across the store, whose offset needs an address temporary.
; The temporary is pushed and popped around the store. A large frame and a
; large outgoing argument area do not matter, since no frame slot is used.
define void @register_pressure() "frame-pointer"="all" {
; No register is free, but the base (r0) is not the stored value, so it is
; adjusted in place and restored rather than spilling an address temporary.
; CHECK-LABEL: register_pressure:
; CHECK:       addw r0, r0, #1024
; CHECK-NEXT:  strt r1, [r0]
; CHECK-NEXT:  subw r0, r0, #1024
  %a = alloca [4096 x i8], align 4
  call void @use(ptr byval([4096 x i8]) %a)
  %regs = call %Registers asm sideeffect "", "={r0},={r1},={r2},={r3},={r4},={r5},={r8},={r9},={r10},={r11},={r12},={lr}"()
  %v0 = extractvalue %Registers %regs, 0
  %v1 = extractvalue %Registers %regs, 1
  %v2 = extractvalue %Registers %regs, 2
  %v3 = extractvalue %Registers %regs, 3
  %v4 = extractvalue %Registers %regs, 4
  %v5 = extractvalue %Registers %regs, 5
  %v6 = extractvalue %Registers %regs, 6
  %v7 = extractvalue %Registers %regs, 7
  %v8 = extractvalue %Registers %regs, 8
  %v9 = extractvalue %Registers %regs, 9
  %v10 = extractvalue %Registers %regs, 10
  %v11 = extractvalue %Registers %regs, 11
  %p = inttoptr i32 %v0 to ptr
  %dst = getelementptr i8, ptr %p, i32 1024
  store i32 %v1, ptr %dst
  call void asm sideeffect "", "{r0},{r1},{r2},{r3},{r4},{r5},{r8},{r9},{r10},{r11},{r12},{lr}"(i32 %v0, i32 %v1, i32 %v2, i32 %v3, i32 %v4, i32 %v5, i32 %v6, i32 %v7, i32 %v8, i32 %v9, i32 %v10, i32 %v11)
  call void @use(ptr byval([4096 x i8]) %a)
  ret void
}
