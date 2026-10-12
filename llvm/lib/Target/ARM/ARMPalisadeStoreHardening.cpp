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
#include "ARMSubtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/LiveRegUnits.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
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
  const TargetRegisterInfo &TRI;
  const LiveRegUnits &Live;
  LiveRegUnits Unavailable;
  ARMCC::CondCodes Pred;
  Register PredReg;
  Register AddressReg, DataReg;
  SmallVector<Register, NumTemporaries> Saved;

  Register temporary() {
    // Prefer dead registers; otherwise save a victim that the original store
    // neither reads nor defines. Reserved registers include SP, PC and FP.
    for (bool Spill : {false, true}) {
      for (MCPhysReg Reg : ARM::rGPRRegClass) {
        if (MF.getRegInfo().isReserved(Reg) || !Unavailable.available(Reg) ||
            (!Spill && !Live.available(Reg)))
          continue;
        Unavailable.addReg(Reg);
        if (Spill)
          Saved.push_back(Reg);
        return Reg;
      }
    }
    reportUnsupportedStore(MI, "no scratch register outside store operands");
  }

  MachineInstrBuilder build(unsigned Opcode) {
    return BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII.get(Opcode));
  }

  // Splitting an undef FP operand into S registers must retain its undef flag.
  RegState useFlags(Register Reg) const {
    bool Undef = false;
    for (const MachineOperand &MO : MI.operands()) {
      if (!MO.isReg() || !MO.isUse() || !MO.getReg() ||
          !TRI.regsOverlap(Reg, MO.getReg()))
        continue;
      if (!MO.isUndef())
        return {};
      Undef = true;
    }
    return getUndefRegState(Undef);
  }

  unsigned pushedBytes() const { return 4 * Saved.size(); }

  // The save and the restore both access the words just below the SP that
  // was current before the save.
  MachineMemOperand *savedWords(MachineMemOperand::Flags Flags) {
    return MF.getMachineMemOperand(
        MachinePointerInfo::getStack(MF, -int(pushedBytes())), Flags,
        pushedBytes(), Align(4));
  }

  // The wide STM/LDM encodings require at least two registers. A single
  // register uses the indexed STR/LDR encoding instead.
  void saveTemporaries() {
    if (Saved.empty())
      return;
    llvm::sort(Saved, [&](Register A, Register B) {
      return TRI.getEncodingValue(A) < TRI.getEncodingValue(B);
    });
    if (Saved.size() == 1) {
      build(ARM::t2STR_PRE)
          .addDef(ARM::SP)
          .addReg(Saved.front())
          .addReg(ARM::SP)
          .addImm(-4)
          .add(predOps(ARMCC::AL))
          .addMemOperand(savedWords(MachineMemOperand::MOStore));
      return;
    }
    auto Save = build(ARM::t2STMDB_UPD)
                    .addDef(ARM::SP)
                    .addReg(ARM::SP)
                    .add(predOps(ARMCC::AL));
    for (Register Reg : Saved)
      Save.addReg(Reg);
    Save.addMemOperand(savedWords(MachineMemOperand::MOStore));
  }

  void restoreTemporaries() {
    if (Saved.empty())
      return;
    if (Saved.size() == 1) {
      build(ARM::t2LDR_POST)
          .addDef(Saved.front())
          .addDef(ARM::SP)
          .addReg(ARM::SP)
          .addImm(4)
          .add(predOps(ARMCC::AL))
          .addMemOperand(savedWords(MachineMemOperand::MOLoad));
      return;
    }
    auto Restore = build(ARM::t2LDMIA_UPD)
                       .addDef(ARM::SP)
                       .addReg(ARM::SP)
                       .add(predOps(ARMCC::AL));
    for (Register Reg : Saved)
      Restore.addDef(Reg);
    Restore.addMemOperand(savedWords(MachineMemOperand::MOLoad));
  }

  static bool fitsOffset(int Offset, int Span) {
    return Offset >= 0 && Offset + Span <= 255;
  }

  // Decide on both temporaries before emitting any instructions. Saving the
  // data temporary can itself force an SP-relative address out of STRT range.
  void prepare(ArrayRef<Register> Values, Register Base, int &Offset,
               int Span = 0, bool NeedsAddress = false) {
    // The compiler never stores below SP, where the temporaries are saved.
    if (Base == ARM::SP && Offset < 0)
      reportUnsupportedStore(MI, "store below SP");
    // STRT can store neither SP nor FP registers.
    if (llvm::any_of(Values, [](Register Value) {
          return Value == ARM::SP || !ARM::GPRRegClass.contains(Value);
        }))
      DataReg = temporary();
    int Adjusted = Offset + (Base == ARM::SP ? pushedBytes() : 0);
    if (NeedsAddress || !fitsOffset(Adjusted, Span))
      AddressReg = temporary();
    if (Base == ARM::SP)
      Offset += pushedBytes();
    saveTemporaries();
  }

  // A separate result avoids corrupting the value when source == base/index.
  void addOffset(Register Dest, Register Base, int Offset) {
    // Accounting for a push can take a maximal SP offset beyond ADDW range.
    if (Offset > 4095) {
      addOffset(Dest, Base, 4095);
      addOffset(Dest, Dest, Offset - 4095);
      return;
    }
    assert(Offset >= -4095);
    build(Offset < 0 ? ARM::t2SUBri12 : ARM::t2ADDri12)
        .addDef(Dest)
        .addReg(Base, useFlags(Base))
        .addImm(std::abs(Offset))
        .add(predOps(Pred, PredReg));
  }

  // Rebase once so that stores from Offset to Offset + Span fit the unsigned
  // 8-bit offset of STRT.
  void fitOffset(Register &Base, int &Offset, int Span = 0) {
    if (fitsOffset(Offset, Span))
      return;
    assert(AddressReg && "address temporary was not prepared");
    addOffset(AddressReg, Base, Offset);
    Base = AddressReg;
    Offset = 0;
  }

  void emitStore(unsigned Opcode, Register Value, Register Base, int Offset) {
    // STR can store SP (for example, llvm.stacksave), but STRT requires rGPR.
    if (Value == ARM::SP) {
      assert(DataReg && "data temporary was not prepared");
      Value = DataReg;
      if (pushedBytes())
        addOffset(Value, ARM::SP, pushedBytes());
      else
        build(ARM::tMOVr)
            .addDef(Value)
            .addReg(ARM::SP, useFlags(ARM::SP))
            .add(predOps(Pred, PredReg));
    }
    fitOffset(Base, Offset);
    // Retain the original memory references conservatively on split stores.
    // A combined instruction's MMO order need not describe its register order.
    build(Opcode)
        .addReg(Value, useFlags(Value))
        .addReg(Base, useFlags(Base))
        .addImm(Offset)
        .add(predOps(Pred, PredReg))
        .setMemRefs(MI.memoperands())
        .setMIFlags(MI.getFlags());
  }

  // Store the bits of an S register; VMOVRS does not perform a conversion.
  void emitFPWord(Register Word, Register Base, int Offset) {
    assert(DataReg && "data temporary was not prepared");
    build(ARM::VMOVRS)
        .addDef(DataReg)
        .addReg(Word, useFlags(Word))
        .add(predOps(Pred, PredReg));
    emitStore(ARM::t2STRT, DataReg, Base, Offset);
  }

  void emitFPStore(Register Value, Register Base, int Offset) {
    if (!ARM::DPRRegClass.contains(Value))
      return emitFPWord(Value, Base, Offset);
    Register Lo = TRI.getSubReg(Value, ARM::ssub_0);
    Register Hi = TRI.getSubReg(Value, ARM::ssub_1);
    assert(Lo && Hi && "M-profile D registers consist of two S registers");
    if (MF.getDataLayout().isBigEndian())
      std::swap(Lo, Hi);
    emitFPWord(Lo, Base, Offset);
    emitFPWord(Hi, Base, Offset + 4);
  }

  void lowerImmediate(unsigned Opcode, unsigned Scale = 1) {
    Register Value = MI.getOperand(0).getReg();
    Register Base = MI.getOperand(1).getReg();
    int Offset = MI.getOperand(2).getImm() * Scale;
    prepare(Value, Base, Offset);
    emitStore(Opcode, Value, Base, Offset);
  }

  void lowerRegisterOffset(unsigned Opcode, bool Shifted) {
    Register Value = MI.getOperand(0).getReg();
    Register Base = MI.getOperand(1).getReg();
    int Offset = 0;
    prepare(Value, Base, Offset, 0, /*NeedsAddress=*/true);
    build(ARM::t2ADDrs)
        .addDef(AddressReg)
        .addReg(Base, useFlags(Base))
        .addReg(MI.getOperand(2).getReg(), useFlags(MI.getOperand(2).getReg()))
        .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl,
                                    Shifted ? MI.getOperand(3).getImm() : 0))
        .add(predOps(Pred, PredReg))
        .add(condCodeOp());
    emitStore(Opcode, Value, AddressReg, Offset);
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
    SmallVector<Register, 2> Values = {MI.getOperand(1).getReg()};
    if (Double)
      Values.push_back(MI.getOperand(2).getReg());
    int StoreOffset = 0;
    prepare(Values, Base, StoreOffset, Double ? 4 : 0);
    // A writeback store of its own base is UNPREDICTABLE, so updating the base
    // first cannot change the stored data.
    if (PreIndex)
      addOffset(Base, Base, Offset);
    emitStore(Opcode, Values[0], Base, 0);
    if (Double)
      emitStore(Opcode, Values[1], Base, 4);
    if (!PreIndex)
      addOffset(Base, Base, Offset);
  }

  void lowerDouble() {
    Register Base = MI.getOperand(2).getReg();
    int Offset = MI.getOperand(3).getImm();
    Register Values[] = {MI.getOperand(0).getReg(), MI.getOperand(1).getReg()};
    prepare(Values, Base, Offset, 4);
    fitOffset(Base, Offset, 4);
    emitStore(ARM::t2STRT, Values[0], Base, Offset);
    emitStore(ARM::t2STRT, Values[1], Base, Offset + 4);
  }

  void lowerFP() {
    Register Value = MI.getOperand(0).getReg();
    Register Base = MI.getOperand(1).getReg();
    unsigned Imm = MI.getOperand(2).getImm();
    int Offset = ARM_AM::getAM5Offset(Imm) * 4;
    if (ARM_AM::getAM5Op(Imm) == ARM_AM::sub)
      Offset = -Offset;
    int Span = ARM::DPRRegClass.contains(Value) ? 4 : 0;
    prepare(Value, Base, Offset, Span);
    fitOffset(Base, Offset, Span);
    emitFPStore(Value, Base, Offset);
  }

  void lowerMultiple(bool Decrement) {
    // Only writeback forms define a register: the updated base.
    bool Writeback = MI.getNumExplicitDefs() != 0;
    Register Base = MI.getOperand(Writeback ? 1 : 0).getReg();
    if (Writeback)
      checkWriteback(Base);
    // The register list follows the base and the two predicate operands.
    SmallVector<Register, 16> Values;
    for (const MachineOperand &MO : llvm::drop_begin(
             MI.explicit_operands(), MI.findFirstPredOperandIdx() + 2))
      Values.push_back(MO.getReg());
    int Size = 0;
    for (Register Value : Values)
      Size += ARM::DPRRegClass.contains(Value) ? 8 : 4;
    Register Address = Base;
    int Offset = Decrement ? -Size : 0;
    prepare(Values, Base, Offset, Size - 4);
    fitOffset(Address, Offset, Size - 4);
    for (Register Value : Values) {
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
  StoreLowering(MachineInstr &MI, const LiveRegUnits &Live)
      : MI(MI), MF(*MI.getMF()),
        TII(*MF.getSubtarget<ARMSubtarget>().getInstrInfo()),
        TRI(*MF.getSubtarget().getRegisterInfo()), Live(Live),
        Unavailable(TRI) {
    Pred = getInstrPredicate(MI, PredReg);
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg() && MO.getReg())
        Unavailable.addReg(MO.getReg());
  }

  void run() {
    lower();
    restoreTemporaries();
  }

private:
  // Cases cite the Armv8-M Architecture Reference Manual (DDI0553B.z),
  // section C2.4, by instruction and encoding.
  void lower() {
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

  const TargetRegisterInfo &TRI = *ST.getRegisterInfo();
  const TargetInstrInfo &TII = *ST.getInstrInfo();

  // Collect the original stores, with the registers live at each, before
  // inserting or erasing instructions.
  SmallVector<std::pair<MachineInstr *, LiveRegUnits>, 32> Stores;
  for (MachineBasicBlock &MBB : MF) {
    LiveRegUnits Live(TRI);
    Live.addLiveOuts(MBB);
    // Standalone MIR may lack callee-save information. In that case none of
    // the callee-saved registers can be assumed free.
    if (!MF.getFrameInfo().isCalleeSavedInfoValid())
      for (const MCPhysReg *Reg = MF.getRegInfo().getCalleeSavedRegs();
           Reg && *Reg; ++Reg)
        Live.addReg(*Reg);

    for (MachineInstr &MI : llvm::reverse(MBB)) {
      if (MI.isDebugInstr())
        continue;
      // Conditional definitions cannot kill values live on the false path.
      if (TII.isPredicated(MI))
        Live.accumulate(MI);
      else
        Live.stepBackward(MI);
      if (!needsHardening(MI))
        continue;
      if (getStoreDomain(MI) == StoreDomain::Mixed)
        reportUnsupportedStore(MI, "store crosses protection domains");
      Stores.emplace_back(&MI, Live);
    }
  }

  // Lowering a store does not change the registers live at any other store:
  // its temporaries are dead there, or saved and restored around it.
  for (auto &[MI, Live] : Stores) {
    StoreLowering(*MI, Live).run();
    MI->eraseFromParent();
  }
  return !Stores.empty();
}

FunctionPass *llvm::createARMPalisadeStoreHardeningPass() {
  return new ARMPalisadeStoreHardening();
}
