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
// This pass transform untrusted ordinary store instructions to unprivileged
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
#include "llvm/Support/ErrorHandling.h"
#include <cstdlib>

using namespace llvm;

#define DEBUG_TYPE "arm-palisade-store-hardening"

namespace {

constexpr unsigned ProtectedAddressSpace = 200;

// The maximal temporaries stack frame space needed for registers spill is 2.
// One for address related temporary (large offset),
// and one data temporary (FP or SP).
constexpr unsigned NumTemporaries = 2;

enum class StoreDomain { Ordinary, Privileged, Mixed };

StoreDomain getStoreDomain(const MachineInstr &MI) {
  bool Ordinary = false, Protected = false;

  for (const MachineMemOperand *MMO : MI.memoperands()) {
    // Only store at explicit IR level will be hardened.
    // PseudoSourceValue stores like memory spills uses STR like normal.
    if (!MMO->isStore() || !MMO->getValue())
      continue;

    if (MMO->getAddrSpace() == ProtectedAddressSpace)
      Protected = true;
    else
      Ordinary = true;
  }

  // Compiler optimizations might group store instructions;
  // instructions might have multiple address domain.
  if (Ordinary && Protected)
    return StoreDomain::Mixed;

  return Ordinary ? StoreDomain::Ordinary : StoreDomain::Privileged;
}

bool isUnprivilegedStore(unsigned Opcode) {
  return Opcode == ARM::t2STRT || Opcode == ARM::t2STRHT ||
         Opcode == ARM::t2STRBT;
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

// Whether a MI needs to be hardened.
bool needsHardening(const MachineInstr &MI) {
  StoreDomain Domain = getStoreDomain(MI);

  // Compiler optimizations may group multiple store instructions into one,
  // though it is rare to group stores in different address space,
  // but if it occurs, we choose to raise Exceptions at compile-time,
  // because a store now can touch multiple protection domain.
  if (Domain == StoreDomain::Mixed)
    reportUnsupportedStore(MI, "store crosses protection domains");

  // A MI needs to be hardened if:
  // 1. may write to the memory,
  // 2. not a (bundled) call instruction,
  // 3. not inline assembly (we do not handle),
  // 4. not already a unprivileged store,
  // 5. store to a valid ordinary address domain.
  return MI.mayStore() && !MI.isCall() && !MI.isInlineAsm() &&
         !isUnprivilegedStore(MI.getOpcode()) &&
         Domain == StoreDomain::Ordinary;
}

// Transforming a privileged store into unprivileged store.
// The transformation requires more work than replacing opcode.
// In Armv8-M, privileged store (STR) is more capable than
// unprivileged store (STRT).
// The transformation needs to work around the following cases due to
// the limitation of STRT:
// 1. operand uses a large offset other than 0-255.
// 2. operand uses SP related offset.
// 3. instruction update its base as well such as, str r1, [r0, #4]!, [r0], #4
// 4. store multiple registers, such as STRD, STM
// 5. operate on FP register, such as VSTR s0
// Additionally,
// transforming to STRT might add instructions, thus it requires free temporary
// registers to encode or,
// pick temporary registers to encode and restore them later.
class StoreLowering {
  MachineInstr &MI;
  MachineFunction &MF;
  const ARMBaseInstrInfo &TII;
  const TargetRegisterInfo &TRI;

  // Registers holding values needed after MI, computed before MI (live)
  // A temporary taken from these must be saved and restored.
  const LiveRegUnits &Live;

  // Registers that cannot become temporaries: every operand of MI and the
  // temporaries already chosen.
  LiveRegUnits Unavailable;

  // MI's predicate, applied to every emitted instruction except the
  // temporary saves and restores.
  ARMCC::CondCodes Pred;
  Register PredReg;

  // Temporaries registers needed by STRT transformation:
  // 1. address range
  // 2. value that is SP, FP related
  Register AddressReg, DataReg;

  // Register value that saved in stack frame in order to make
  // temporaries registers available.
  // They are pushed before lowering store and popped after it.
  SmallVector<Register, NumTemporaries> Saved;

  // Choose a register for a temporary. Prefer one that holds no live value,
  // otherwise borrow a live one and save its value around the store.
  Register temporary() {
    // A register is not reserved (such as the frame pointer) and is not used
    // by MI or an earlier temporary.
    auto IsCandidate = [&](MCPhysReg Reg) {
      return !MF.getRegInfo().isReserved(Reg) && Unavailable.available(Reg);
    };

    // Found a live register that is available to use.
    for (MCPhysReg Reg : ARM::rGPRRegClass) {
      if (IsCandidate(Reg) && Live.available(Reg)) {
        Unavailable.addReg(Reg);
        return Reg;
      }
    }

    // Not found live register,
    // pick one candidate as temporary register and save its value.
    for (MCPhysReg Reg : ARM::rGPRRegClass) {
      if (IsCandidate(Reg)) {
        Unavailable.addReg(Reg);
        Saved.push_back(Reg);
        return Reg;
      }
    }

    reportUnsupportedStore(MI, "no scratch register outside store operands");
  }

  MachineInstrBuilder build(unsigned Opcode) {
    return BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII.get(Opcode));
  }

  // The undef flag for a new read of Reg: set only if every read of Reg (or of
  // a register overlapping it) in MI is undef.
  // The flag must carry over for instructmented instructions, or the
  // verifier rejects the read.
  RegState undefState(Register Reg) const {
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

  // Number of bytes we need to move SP register.
  unsigned pushedBytes() const { return 4 * Saved.size(); }

  // Builds the memory operand attached to the push (MOStore) and the pop (MOLoad).
  MachineMemOperand *savedWords(MachineMemOperand::Flags Flags) {
    return MF.getMachineMemOperand(
        MachinePointerInfo::getStack(MF, -int(pushedBytes())), Flags,
        pushedBytes(), Align(4));
  }

  // Build push instruction to save temporarie on stack.
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

  // Build pop instruction to restore temporaries.
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

  // Whether words stored at Offset through Offset + Span all fit the unsigned
  // 8-bit immediate offset of STRT (0-255).
  static bool fitsSTRTOffset(int Offset, int Span) {
    return Offset >= 0 && Offset + Span <= 255;
  }

  // Choose the temporaries this store needs and push those that hold live
  // values. However, pushing the data temporary can change an SP-relative store
  // semantics, so an SP-relative Offset is rebased here.
  void reserveTemporaries(ArrayRef<Register> Values, Register Base, int &Offset,
                          int Span = 0, bool RegisterOffset = false) {
    // The compiler never stores below SP, where the temporaries are saved.
    if (Base == ARM::SP && Offset < 0)
      reportUnsupportedStore(MI, "store below SP");

    // Save data temporary
    if (llvm::any_of(Values, [](Register Value) {
          return Value == ARM::SP || !ARM::GPRRegClass.contains(Value);
        }))
      DataReg = temporary();

    // Save address temporary
    int Adjusted = Offset + (Base == ARM::SP ? pushedBytes() : 0);

    // A register-offset address (base + index) is computed into the
    // address temporary or offset STRT cannot encode.
    if (RegisterOffset || !fitsSTRTOffset(Adjusted, Span))
      AddressReg = temporary();

    // Rebase SP offset
    if (Base == ARM::SP)
      Offset += pushedBytes();

    // push temporaries into stack
    saveTemporaries();
  }

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
        .addReg(Base, undefState(Base))
        .addImm(std::abs(Offset))
        .add(predOps(Pred, PredReg));
  }

  // Rewrite Base + Offset into an address STRT can encode. If the words at
  // Offset through Offset + Span are out of range, compute Base + Offset into
  // the address temporary and continue from [AddressReg, #0].
  void legalizeSTRTAddress(Register &Base, int &Offset, int Span = 0) {
    if (fitsSTRTOffset(Offset, Span))
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
      // Store SP as it was before any temporaries were pushed.
      addOffset(Value, ARM::SP, pushedBytes());
    }
    assert(fitsSTRTOffset(Offset, 0) && "STRT address was not legalized");
    // Retain the original memory references on split stores.
    build(Opcode)
        .addReg(Value, undefState(Value))
        .addReg(Base, undefState(Base))
        .addImm(Offset)
        .add(predOps(Pred, PredReg))
        .setMemRefs(MI.memoperands())
        .setMIFlags(MI.getFlags());
  }

  // Emit 32 bits store.
  // Store the bits of an S register, VMOVRS does not perform a conversion.
  void emitFPWord(Register Word, Register Base, int Offset) {
    assert(DataReg && "data temporary was not prepared");
    build(ARM::VMOVRS)
        .addDef(DataReg)
        .addReg(Word, undefState(Word))
        .add(predOps(Pred, PredReg));
    emitStore(ARM::t2STRT, DataReg, Base, Offset);
  }

  // Split 64 bit store if using D registers
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
    reserveTemporaries(Value, Base, Offset);
    legalizeSTRTAddress(Base, Offset);
    emitStore(Opcode, Value, Base, Offset);
  }

  void lowerRegisterOffset(unsigned Opcode, bool Shifted) {
    Register Value = MI.getOperand(0).getReg();
    Register Base = MI.getOperand(1).getReg();
    Register Index = MI.getOperand(2).getReg();
    int Offset = 0;
    reserveTemporaries(Value, Base, Offset, 0, /*RegisterOffset=*/true);
    build(ARM::t2ADDrs)
        .addDef(AddressReg)
        .addReg(Base, undefState(Base))
        .addReg(Index, undefState(Index))
        .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl,
                                    Shifted ? MI.getOperand(3).getImm() : 0))
        .add(predOps(Pred, PredReg))
        .add(condCodeOp());
    emitStore(Opcode, Value, AddressReg, Offset);
  }


  // Some Store instructions also update its base register (writeback),
  // for examples:
  // 1. pre-indexed: str r1, [r0, #4]!, r0 += 4 then update r0,
  // 2. post-indexed:  str r1, [r0], #4, update r0, then r0 += 4,
  // 3. multiple:  stmdb r0!, {r1, r2}, store then r0 -= 8
  void checkWriteback(Register Base) {
    // Guard:
    // prevent writeback to SP, so we do not change interrupt code, which
    // might use writeback instructions to push exception frame
    // after current frame.
    if (Base == ARM::SP)
      reportUnsupportedStore(MI, "store with SP writeback");
  }

  void lowerIndexed(unsigned Opcode, bool PreIndex, bool Double = false) {
    // Base operand index
    unsigned BaseOp = Double ? 3 : 2;
    Register Base = MI.getOperand(BaseOp).getReg();
    int Offset = MI.getOperand(BaseOp + 1).getImm();
    checkWriteback(Base);
    SmallVector<Register, 2> Values = {MI.getOperand(1).getReg()};
    if (Double)
      Values.push_back(MI.getOperand(2).getReg());
    int StoreOffset = 0;
    reserveTemporaries(Values, Base, StoreOffset, Double ? 4 : 0);

    // Emulating the writeback behavior

    if (PreIndex)
      // Base = Base + Offset
      addOffset(Base, Base, Offset);
    // Store at Base + 0
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
    reserveTemporaries(Values, Base, Offset, 4);
    legalizeSTRTAddress(Base, Offset, 4);
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
    reserveTemporaries(Value, Base, Offset, Span);
    legalizeSTRTAddress(Base, Offset, Span);
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

    // Calculating memory size and direction.
    int Size = 0;
    for (Register Value : Values)
      Size += ARM::DPRRegClass.contains(Value) ? 8 : 4;

    Register Address = Base;
    int Offset = Decrement ? -Size : 0;

    reserveTemporaries(Values, Base, Offset, Size - 4);
    legalizeSTRTAddress(Address, Offset, Size - 4);

    // Emit individual store
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

  // Transform the STR to STRT,
  // restore the registers to memory if needed.
  void run() {
    lower();
    restoreTemporaries();
  }

private:
  // Lowering  STR to STRT based on Armv8-M
  // Architecture Reference Manual (DDI0553B.z),
  // section C2.4, by instruction and encoding.
  // Each case cites the instruction's section and the encodings it covers.
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
    // Other stores in C2.4 are not lowered/implemented:
    //
    // 1. STL, STLB, STLH, STLEX, STLEXB, STLEXH (C2.4.216-221) and STREX,
    //   STREXB, STREXH (C2.4.230-232): no unprivileged release or exclusive
    //   store exists.
    //
    // 2. VSTR T3 (.16, C2.4.483), FSTMDBX and FSTMIAX (C2.4.64), STC and STC2
    //   (C2.4.215).
    //
    // 3. MVE stores: VST2, VST4 (C2.4.480-481) and VSTRB, VSTRH, VSTRW, VSTRD
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

  // Check valid target and ISA.
  if (!ST.isThumb2() || !ST.isMClass())
    report_fatal_error("Palisade store hardening requires Thumb-2 M-profile",
                       false);

  const TargetRegisterInfo &TRI = *ST.getRegisterInfo();
  const TargetInstrInfo &TII = *ST.getInstrInfo();

  // Iterating MachineInstr to find store instructions that needs harden.
  SmallVector<std::pair<MachineInstr *, LiveRegUnits>, 32> Stores;
  for (MachineBasicBlock &MBB : MF) {

    // A set of registers whose value needed by later instructions.
    // A register outside this set is free to use.
    LiveRegUnits Live(TRI);

    // registers needed by later Basic Blocks (live-out).
    Live.addLiveOuts(MBB);

    // Standalone MIR (needed by tests) may lack callee-save information.
    // In that case, assume none of the callee-saved registers are free.
    if (!MF.getFrameInfo().isCalleeSavedInfoValid())
      for (const MCPhysReg *Reg = MF.getRegInfo().getCalleeSavedRegs();
           Reg && *Reg; ++Reg)
        Live.addReg(*Reg);

    // Iterate instructions backward from Basic Block.
    // Therefore, if a instruction reads(uses) a register, it become live.
    // If a instruction writes(defines) a register, it become dead.
    for (MachineInstr &MI : llvm::reverse(MBB)) {

      if (MI.isDebugInstr())
        continue;

      // A predicate instruction such as MOVEQ only executes if a specific
      // condition flag is met.
      if (TII.isPredicated(MI))
        // We need to conservatively assume when condition is not met:
        // accumulate every register this instruction may touches.
        Live.accumulate(MI);
      else
        // Otherwise, remove register this instruction defined,
        // add registers this instruction uses.
        Live.stepBackward(MI);

      if (!needsHardening(MI))
        continue;

      Stores.emplace_back(&MI, Live);
    }
  }

  // Harden the store instructions.
  for (auto &[MI, Live] : Stores) {
    StoreLowering(*MI, Live).run();
    MI->eraseFromParent();
  }

  return !Stores.empty();
}

FunctionPass *llvm::createARMPalisadeStoreHardeningPass() {
  return new ARMPalisadeStoreHardening();
}
