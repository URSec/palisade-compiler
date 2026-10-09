//===- ARMPalisadeStoreHardening.cpp - Unprivileged stores conversion------===//
//
//     Lowering ordinary stores to unprivileged stores.
//     Copyright (c) 2026, Pengxiang Huang, University of Rochester
//
// Part of the palisade project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.txt in the top-level directory for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass transform ordinary store instructions to to the unprivileged
// stores for Armv8-M target.
//
//===----------------------------------------------------------------------===//

//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//              Copyright (c) 2019-2020, University of Rochester
//
// Part of the Silhouette Project, under the Apache License v2.0 with
// LLVM Exceptions.
// See LICENSE.txt in the top-level directory for license information.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMMachineFunctionInfo.h"
#include "ARMSubtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdlib>

using namespace llvm;

#define DEBUG_TYPE "arm-palisade-store-hardening"

namespace {

constexpr unsigned ProtectedAddressSpace = 200;

// At most an address and one data temporary (FP bits or SP) are live together
// while lowering a store.
constexpr unsigned NumTemporaries = 2;

enum class StoreDomain { Ordinary, Privileged, Mixed };

// The physical base register alone cannot establish a store's domain. In
// particular, ordinary allocas still use SP until stack splitting is enabled.
bool isPrivilegedMemory(const MachineMemOperand &MMO,
                        const MachineFrameInfo &MFI) {
  if (MMO.getAddrSpace() == ProtectedAddressSpace)
    return true;
  const PseudoSourceValue *PSV = MMO.getPseudoValue();
  if (!PSV)
    return false;
  if (const auto *FS = dyn_cast<FixedStackPseudoSourceValue>(PSV)) {
    int FI = FS->getFrameIndex();
    if (const AllocaInst *AI = MFI.getObjectAllocation(FI))
      return AI->getAddressSpace() == ProtectedAddressSpace;
    return MFI.isSpillSlotObjectIndex(FI) || MFI.isFixedObjectIndex(FI);
  }
  return PSV->isStack(); // Outgoing ABI arguments.
}

bool isFrameSave(const MachineInstr &MI) {
  if (!MI.getFlag(MachineInstr::FrameSetup))
    return false;
  switch (MI.getOpcode()) {
  case ARM::tPUSH:
    return true;
  case ARM::t2STMDB_UPD:
  case ARM::t2STR_PRE:
  case ARM::VSTMDDB_UPD:
  case ARM::VSTMSDB_UPD:
    return MI.getOperand(0).getReg() == ARM::SP;
  default:
    return false;
  }
}

StoreDomain getStoreDomain(const MachineInstr &MI) {
  bool Ordinary = false, Privileged = false;
  for (const MachineMemOperand *MMO : MI.memoperands()) {
    if (!MMO->isStore())
      continue;
    if (isPrivilegedMemory(*MMO, MI.getMF()->getFrameInfo()))
      Privileged = true;
    else
      Ordinary = true;
  }
  if (Ordinary && Privileged)
    return StoreDomain::Mixed;
  // Missing memory operands never confer privilege on a program store.
  if (Privileged || (!Ordinary && isFrameSave(MI)))
    return StoreDomain::Privileged;
  return StoreDomain::Ordinary;
}

bool isUnprivilegedStore(unsigned Opcode) {
  return Opcode == ARM::t2STRT || Opcode == ARM::t2STRHT ||
         Opcode == ARM::t2STRBT;
}

// Whether MI is a program store that this pass must lower or reject. Inline
// assembly is trusted, like hand-written runtime code, and never rewritten.
bool needsHardening(const MachineInstr &MI) {
  return MI.mayStore() && !MI.isCall() && !MI.isInlineAsm() &&
         !isUnprivilegedStore(MI.getOpcode()) &&
         getStoreDomain(MI) != StoreDomain::Privileged;
}

// Terminate compilation when Palisade cannot harden an instruction.
[[noreturn]] void reportUnsupportedStore(const MachineInstr &MI,
                                         StringRef Reason) {
  const auto &TII = *MI.getMF()->getSubtarget().getInstrInfo();
  report_fatal_error(Twine("Palisade store hardening: ") + Reason + " (" +
                         TII.getName(MI.getOpcode()) + ") in function '" +
                         MI.getMF()->getName() + "'",
                     false);
}

class StoreLowering {
  MachineInstr &MI;
  MachineFunction &MF;
  const ARMBaseInstrInfo &TII;
  ARMCC::CondCodes Pred;
  Register PredReg;

  Register temporary() {
    MF.getProperties().resetNoVRegs();
    return MF.getRegInfo().createVirtualRegister(&ARM::GPRRegClass);
  }

  MachineInstrBuilder build(unsigned Opcode) {
    return BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII.get(Opcode));
  }

  // All offsets in the supported store encodings fit an ADDW/SUBW immediate.
  // A separate result avoids corrupting the value when source == base/index.
  void addOffset(Register Dest, Register Base, int Offset) {
    assert(Offset >= -4095 && Offset <= 4095);
    build(Offset < 0 ? ARM::t2SUBri12 : ARM::t2ADDri12)
        .addDef(Dest)
        .addReg(Base)
        .addImm(std::abs(Offset))
        .add(predOps(Pred, PredReg));
  }

  Register offsetAddress(Register Base, int Offset) {
    if (!Offset)
      return Base;
    Register Address = temporary();
    addOffset(Address, Base, Offset);
    return Address;
  }

  // Rebase once so that stores from Offset to Offset + Span fit the unsigned
  // 8-bit offset of STRT.
  void fitOffset(Register &Base, int &Offset, int Span = 0) {
    if (Offset >= 0 && Offset + Span <= 255)
      return;
    Base = offsetAddress(Base, Offset);
    Offset = 0;
  }

  void emitStore(unsigned Opcode, Register Value, Register Base, int Offset) {
    // STR can store SP (for example, llvm.stacksave), but STRT requires rGPR.
    if (Value == ARM::SP) {
      Value = temporary();
      build(ARM::tMOVr)
          .addDef(Value)
          .addReg(ARM::SP)
          .add(predOps(Pred, PredReg));
    }
    fitOffset(Base, Offset);
    // Retain the original memory references conservatively on split stores.
    // A combined instruction's MMO order need not describe its register order.
    build(Opcode)
        .addReg(Value)
        .addReg(Base)
        .addImm(Offset)
        .add(predOps(Pred, PredReg))
        .setMemRefs(MI.memoperands())
        .setMIFlags(MI.getFlags());
  }

  // Store the bits of an S register; VMOVRS does not perform a conversion.
  void emitFPWord(Register Word, Register Base, int Offset) {
    Register Bits = temporary();
    build(ARM::VMOVRS).addDef(Bits).addReg(Word).add(predOps(Pred, PredReg));
    emitStore(ARM::t2STRT, Bits, Base, Offset);
  }

  void emitFPStore(Register Value, Register Base, int Offset) {
    if (!ARM::DPRRegClass.contains(Value))
      return emitFPWord(Value, Base, Offset);
    const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
    Register Lo = TRI.getSubReg(Value, ARM::ssub_0);
    Register Hi = TRI.getSubReg(Value, ARM::ssub_1);
    assert(Lo && Hi && "M-profile D registers consist of two S registers");
    if (MF.getDataLayout().isBigEndian())
      std::swap(Lo, Hi);
    emitFPWord(Lo, Base, Offset);
    emitFPWord(Hi, Base, Offset + 4);
  }

  void lowerImmediate(unsigned Opcode, unsigned Scale = 1) {
    emitStore(Opcode, MI.getOperand(0).getReg(), MI.getOperand(1).getReg(),
              MI.getOperand(2).getImm() * Scale);
  }

  void lowerRegisterOffset(unsigned Opcode, bool Shifted) {
    Register Address = temporary();
    build(ARM::t2ADDrs)
        .addDef(Address)
        .addReg(MI.getOperand(1).getReg())
        .addReg(MI.getOperand(2).getReg())
        .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl,
                                    Shifted ? MI.getOperand(3).getImm() : 0))
        .add(predOps(Pred, PredReg))
        .add(condCodeOp());
    emitStore(Opcode, MI.getOperand(0).getReg(), Address, 0);
  }

  void checkWriteback(Register Base) {
    // Moving SP around an ordinary store would expose memory below the live
    // stack to interrupts. Compiler frame saves are handled separately.
    if (Base == ARM::SP)
      reportUnsupportedStore(MI, "store with SP writeback");
  }

  void lowerIndexed(unsigned Opcode, bool PreIndex, bool Double = false) {
    unsigned BaseOp = Double ? 3 : 2;
    Register Base = MI.getOperand(BaseOp).getReg();
    int Offset = MI.getOperand(BaseOp + 1).getImm();
    checkWriteback(Base);
    Register Address = PreIndex ? offsetAddress(Base, Offset) : Base;
    emitStore(Opcode, MI.getOperand(1).getReg(), Address, 0);
    if (Double)
      emitStore(Opcode, MI.getOperand(2).getReg(), Address, 4);
    // Update only after the data has been read, including source/base aliases.
    addOffset(MI.getOperand(0).getReg(), Base, Offset);
  }

  void lowerDouble() {
    Register Base = MI.getOperand(2).getReg();
    int Offset = MI.getOperand(3).getImm();
    fitOffset(Base, Offset, 4);
    emitStore(ARM::t2STRT, MI.getOperand(0).getReg(), Base, Offset);
    emitStore(ARM::t2STRT, MI.getOperand(1).getReg(), Base, Offset + 4);
  }

  void lowerFP() {
    Register Value = MI.getOperand(0).getReg();
    Register Base = MI.getOperand(1).getReg();
    unsigned Imm = MI.getOperand(2).getImm();
    int Offset = ARM_AM::getAM5Offset(Imm) * 4;
    if (ARM_AM::getAM5Op(Imm) == ARM_AM::sub)
      Offset = -Offset;
    fitOffset(Base, Offset, ARM::DPRRegClass.contains(Value) ? 4 : 0);
    emitFPStore(Value, Base, Offset);
  }

  void lowerMultiple(bool Decrement) {
    // Only writeback forms define a register: the updated base.
    bool Writeback = MI.getNumExplicitDefs() != 0;
    Register Base = MI.getOperand(Writeback ? 1 : 0).getReg();
    if (Writeback)
      checkWriteback(Base);
    // The register list follows the base and the two predicate operands.
    auto Values = llvm::drop_begin(MI.explicit_operands(),
                                   MI.findFirstPredOperandIdx() + 2);
    int Size = 0;
    for (const MachineOperand &MO : Values)
      Size += ARM::DPRRegClass.contains(MO.getReg()) ? 8 : 4;
    Register Address = Base;
    int Offset = Decrement ? -Size : 0;
    fitOffset(Address, Offset, Size - 4);
    for (const MachineOperand &MO : Values) {
      Register Value = MO.getReg();
      if (ARM::GPRRegClass.contains(Value))
        emitStore(ARM::t2STRT, Value, Address, Offset);
      else
        emitFPStore(Value, Address, Offset);
      Offset += ARM::DPRRegClass.contains(Value) ? 8 : 4;
    }
    if (Writeback)
      addOffset(MI.getOperand(0).getReg(), Base, Decrement ? -Size : Size);
  }

public:
  explicit StoreLowering(MachineInstr &MI)
      : MI(MI), MF(*MI.getMF()),
        TII(*MF.getSubtarget<ARMSubtarget>().getInstrInfo()) {
    Pred = getInstrPredicate(MI, PredReg);
  }

  // Cases cite the Armv8-M Architecture Reference Manual (DDI0553B.z),
  // section C2.4, by instruction and encoding.
  void run() {
    switch (MI.getOpcode()) {
    // C2.4.224 STR (immediate): T1, T2 (SP base), T3, T4.
    case ARM::tSTRi:
    case ARM::tSTRspi:
      return lowerImmediate(ARM::t2STRT, 4);
    case ARM::t2STRi12:
    case ARM::t2STRi8:
      return lowerImmediate(ARM::t2STRT);
    case ARM::t2STR_PRE:
      return lowerIndexed(ARM::t2STRT, /*PreIndex=*/true);
    case ARM::t2STR_POST:
      return lowerIndexed(ARM::t2STRT, /*PreIndex=*/false);
    // C2.4.225 STR (register): T1, T2.
    case ARM::tSTRr:
      return lowerRegisterOffset(ARM::t2STRT, /*Shifted=*/false);
    case ARM::t2STRs:
      return lowerRegisterOffset(ARM::t2STRT, /*Shifted=*/true);
    // C2.4.226 STRB (immediate): T1, T2, T3.
    case ARM::tSTRBi:
    case ARM::t2STRBi12:
    case ARM::t2STRBi8:
      return lowerImmediate(ARM::t2STRBT);
    case ARM::t2STRB_PRE:
      return lowerIndexed(ARM::t2STRBT, /*PreIndex=*/true);
    case ARM::t2STRB_POST:
      return lowerIndexed(ARM::t2STRBT, /*PreIndex=*/false);
    // C2.4.227 STRB (register): T1, T2.
    case ARM::tSTRBr:
      return lowerRegisterOffset(ARM::t2STRBT, /*Shifted=*/false);
    case ARM::t2STRBs:
      return lowerRegisterOffset(ARM::t2STRBT, /*Shifted=*/true);
    // C2.4.233 STRH (immediate): T1, T2, T3.
    case ARM::tSTRHi:
      return lowerImmediate(ARM::t2STRHT, 2);
    case ARM::t2STRHi12:
    case ARM::t2STRHi8:
      return lowerImmediate(ARM::t2STRHT);
    case ARM::t2STRH_PRE:
      return lowerIndexed(ARM::t2STRHT, /*PreIndex=*/true);
    case ARM::t2STRH_POST:
      return lowerIndexed(ARM::t2STRHT, /*PreIndex=*/false);
    // C2.4.234 STRH (register): T1, T2.
    case ARM::tSTRHr:
      return lowerRegisterOffset(ARM::t2STRHT, /*Shifted=*/false);
    case ARM::t2STRHs:
      return lowerRegisterOffset(ARM::t2STRHT, /*Shifted=*/true);
    // C2.4.229 STRD (immediate): T1.
    case ARM::t2STRDi8:
      return lowerDouble();
    case ARM::t2STRD_PRE:
      return lowerIndexed(ARM::t2STRT, /*PreIndex=*/true, /*Double=*/true);
    case ARM::t2STRD_POST:
      return lowerIndexed(ARM::t2STRT, /*PreIndex=*/false, /*Double=*/true);
    // C2.4.222 STM: T1, T2. C2.4.482 VSTM (increment after): T1, T2.
    case ARM::tSTMIA_UPD:
    case ARM::t2STMIA:
    case ARM::t2STMIA_UPD:
    case ARM::VSTMDIA:
    case ARM::VSTMDIA_UPD:
    case ARM::VSTMSIA:
    case ARM::VSTMSIA_UPD:
      return lowerMultiple(/*Decrement=*/false);
    // C2.4.223 STMDB: T1. C2.4.482 VSTM (decrement before): T1, T2.
    case ARM::t2STMDB:
    case ARM::t2STMDB_UPD:
    case ARM::VSTMDDB_UPD:
    case ARM::VSTMSDB_UPD:
      return lowerMultiple(/*Decrement=*/true);
    // C2.4.483 VSTR: T1 (D register), T2 (S register).
    case ARM::VSTRD:
    case ARM::VSTRS:
      return lowerFP();
    // Other stores in C2.4 are not lowered:
    // - STRT, STRBT, STRHT (C2.4.236, .228, .235) are already unprivileged.
    // - PUSH (C2.4.147-148) and VPUSH (C2.4.432) are SP aliases of STMDB, STR,
    //   and VSTM. They stay privileged as frame saves; any other SP writeback
    //   is rejected.
    // Rejected:
    // - STL, STLB, STLH, STLEX, STLEXB, STLEXH (C2.4.216-221) and STREX,
    //   STREXB, STREXH (C2.4.230-232): no unprivileged release or exclusive
    //   store exists.
    // - VSTR T3 (.16, C2.4.483), VSTR (System Register) (C2.4.484), VLSTM
    //   (C2.4.368), FSTMDBX and FSTMIAX (C2.4.64), STC and STC2 (C2.4.215).
    // - MVE stores: VST2, VST4 (C2.4.480-481) and VSTRB, VSTRH, VSTRW, VSTRD
    //   (C2.4.485-486).
    default:
      reportUnsupportedStore(MI, "unsupported store");
    }
  }
};

// Assign physical registers to lowering temporaries, spilling into the
// reserved slots only when no register is free.
void scavengeTemporaries(MachineFunction &MF) {
  if (MF.getProperties().hasNoVRegs())
    return;
  // The normal pipeline runs after PEI; reject standalone invocation without
  // callee-saved information.
  if (!MF.getFrameInfo().isCalleeSavedInfoValid())
    report_fatal_error(
        "Palisade store hardening requires valid callee-saved information",
        false);
  RegScavenger RS;
  for (int FI : MF.getInfo<ARMFunctionInfo>()->getPalisadeSpillSlots())
    RS.addScavengingFrameIndex(FI);
  scavengeFrameVirtualRegs(MF, RS);
}

class ARMPalisadeStoreHardening : public MachineFunctionPass {
public:
  static char ID;
  ARMPalisadeStoreHardening() : MachineFunctionPass(ID) {
    initializeARMPalisadeStoreHardeningPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override {
    return "ARM Palisade store hardening";
  }
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs().setTracksLiveness();
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // end anonymous namespace

char ARMPalisadeStoreHardening::ID = 0;
INITIALIZE_PASS(ARMPalisadeStoreHardening, DEBUG_TYPE,
                "ARM Palisade store hardening", false, false)

bool ARMPalisadeStoreHardening::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget<ARMSubtarget>();
  if (!ST.isThumb2() || !ST.isMClass())
    report_fatal_error("Palisade store hardening requires Thumb-2 M-profile",
                       false);

  // Collect the original stores before inserting or erasing instructions.
  SmallVector<MachineInstr *, 32> Stores;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (!needsHardening(MI))
        continue;
      if (getStoreDomain(MI) == StoreDomain::Mixed)
        reportUnsupportedStore(MI, "store crosses protection domains");
      Stores.push_back(&MI);
    }
  }

  for (MachineInstr *MI : Stores) {
    StoreLowering(*MI).run();
    MI->eraseFromParent();
  }
  scavengeTemporaries(MF);
  return !Stores.empty();
}

void llvm::reservePalisadeSpillSlots(MachineFunction &MF) {
  if (llvm::none_of(MF, [](const MachineBasicBlock &MBB) {
        return llvm::any_of(MBB, needsHardening);
      }))
    return;
  // Do not register these with PEI's scavenger: its spills can be live across
  // the store we later expand. The late scavenger needs its own slots.
  for (unsigned I = 0; I != NumTemporaries; ++I)
    MF.getInfo<ARMFunctionInfo>()->addPalisadeSpillSlot(
        MF.getFrameInfo().CreateSpillStackObject(4, Align(4)));
}

FunctionPass *llvm::createARMPalisadeStoreHardeningPass() {
  return new ARMPalisadeStoreHardening();
}
