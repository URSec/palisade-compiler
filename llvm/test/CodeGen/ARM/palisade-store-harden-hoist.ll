; RUN: llc -mtriple=thumbv8.1m.main-none-eabi -mcpu=cortex-m85 -arm-enable-palisade-store-hardening -verify-machineinstrs %s -o - | FileCheck %s

; A counted, call-free loop whose floating-point store address is an affine
; recurrence is proven safe once, before the loop, with a TTT range check, so
; the store stays a native VSTR instead of VMOV+STRT.
define void @saxpy(ptr noalias %y, ptr noalias %x, float %a, i32 %n) {
; CHECK-LABEL: saxpy:
; Two TTT queries (first and last written byte) and the MRVALID|RW test.
; CHECK:       ttt
; CHECK:       ttt
; CHECK:       #589824
; A failed proof traps.
; CHECK:       .inst.n 0xdefe
; The hot store is native: no unprivileged store remains in the function.
; CHECK-NOT:   strt
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %ph, label %exit
ph:
  br label %loop
loop:
  %i = phi i32 [ 0, %ph ], [ %inc, %loop ]
  %px = getelementptr inbounds float, ptr %x, i32 %i
  %vx = load float, ptr %px
  %py = getelementptr inbounds float, ptr %y, i32 %i
  %vy = load float, ptr %py
  %m = call float @llvm.fmuladd.f32(float %a, float %vx, float %vy)
  store float %m, ptr %py
  %inc = add nuw nsw i32 %i, 1
  %e = icmp eq i32 %inc, %n
  br i1 %e, label %exit, label %loop
exit:
  ret void
}

; Integer stores are left to ordinary store hardening (this pass is FP-only):
; no range check, and the store becomes an unprivileged STRT.
define void @iadd(ptr noalias %y, ptr noalias %x, i32 %n) {
; CHECK-LABEL: iadd:
; CHECK-NOT:   ttt
; CHECK:       strt
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %ph, label %exit
ph:
  br label %loop
loop:
  %i = phi i32 [ 0, %ph ], [ %inc, %loop ]
  %px = getelementptr inbounds i32, ptr %x, i32 %i
  %vx = load i32, ptr %px
  %py = getelementptr inbounds i32, ptr %y, i32 %i
  %vy = load i32, ptr %py
  %s = add i32 %vx, %vy
  store i32 %s, ptr %py
  %inc = add nuw nsw i32 %i, 1
  %e = icmp eq i32 %inc, %n
  br i1 %e, label %exit, label %loop
exit:
  ret void
}

; A scatter store (loaded index) is not an affine recurrence, so its range is
; not loop-invariant: no range check, and it is hardened to STRT as usual.
define void @scatter(ptr noalias %y, ptr noalias %idx, float %a, i32 %n) {
; CHECK-LABEL: scatter:
; CHECK-NOT:   ttt
; CHECK:       strt
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %ph, label %exit
ph:
  br label %loop
loop:
  %i = phi i32 [ 0, %ph ], [ %inc, %loop ]
  %pidx = getelementptr inbounds i32, ptr %idx, i32 %i
  %j = load i32, ptr %pidx
  %py = getelementptr inbounds float, ptr %y, i32 %j
  store float %a, ptr %py
  %inc = add nuw nsw i32 %i, 1
  %e = icmp eq i32 %inc, %n
  br i1 %e, label %exit, label %loop
exit:
  ret void
}

declare float @llvm.fmuladd.f32(float, float, float)
