; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -verify-machineinstrs %s -o - | FileCheck %s

; Signing computes the protected pointer's PAC with PACG.
define i32 @sign(ptr addrspace(200) %p, i32 %modifier) {
; CHECK-LABEL: sign:
; CHECK:       pacg r0, r0, r1
; CHECK-NEXT:  bx lr
  %pac = call i32 @llvm.arm.protected.sign(ptr addrspace(200) %p, i32 %modifier)
  ret i32 %pac
}

; The store through the checked pointer stays after AUTG.
define void @auth(ptr addrspace(200) %p, i32 %pac, i32 %modifier, i32 %x) {
; CHECK-LABEL: auth:
; CHECK:       autg r1, r0, r2
; CHECK-NEXT:  str r3, [r0]
; CHECK-NEXT:  bx lr
  %q = call ptr addrspace(200) @llvm.arm.protected.auth(ptr addrspace(200) %p, i32 %pac, i32 %modifier)
  store i32 %x, ptr addrspace(200) %q
  ret void
}

; A loop-invariant check under a condition stays under it: hoisting it out of
; the loop would fault on iterations that never use the pointer.
define void @conditional(ptr addrspace(200) %p, i32 %pac, i32 %modifier, ptr %flags, i32 %n) {
; CHECK-LABEL: conditional:
; CHECK:       .LBB2_2: @ %loop
; CHECK:       beq .LBB2_1
; CHECK-NEXT:  @ %bb.3: @ %then
; CHECK:       mov r4, r0
; CHECK-NEXT:  autg r1, r4, r2
; CHECK-NEXT:  str.w r12, [r4]
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %flag.addr = getelementptr inbounds i32, ptr %flags, i32 %i
  %flag = load i32, ptr %flag.addr
  %set = icmp ne i32 %flag, 0
  br i1 %set, label %then, label %latch

then:
  %q = call ptr addrspace(200) @llvm.arm.protected.auth(ptr addrspace(200) %p, i32 %pac, i32 %modifier)
  store i32 %i, ptr addrspace(200) %q
  br label %latch

latch:
  %i.next = add nuw i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
