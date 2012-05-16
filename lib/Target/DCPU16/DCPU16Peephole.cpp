//===-- DCPU16Peephole.cpp - DCPU16 Peephole Optimiztions ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// This peephole pass optimizes in the following cases.
// 1. Optimizes redundant sign extends for the following case
//    Transform the following pattern
//    %vreg170<def> = SXTW %vreg166
//    ...
//    %vreg176<def> = COPY %vreg170:subreg_loreg
//
//    Into
//    %vreg176<def> = COPY vreg166
//
//  2. Optimizes redundant negation of predicates.
//     %vreg15<def> = CMPGTrr %vreg6, %vreg2
//     ...
//     %vreg16<def> = NOT_p %vreg15<kill>
//     ...
//     JMP_c %vreg16<kill>, <BB#1>, %PC<imp-def,dead>
//
//     Into
//     %vreg15<def> = CMPGTrr %vreg6, %vreg2;
//     ...
//     JMP_cNot %vreg15<kill>, <BB#1>, %PC<imp-def,dead>;
//
// Note: The peephole pass makes the instrucstions like
// %vreg170<def> = SXTW %vreg166 or %vreg16<def> = NOT_p %vreg15<kill>
// redundant and relies on some form of dead removal instrucions, like
// DCE or DIE to actually eliminate them.


//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "DCPU16-peephole"
#include "DCPU16.h"
#include "DCPU16TargetMachine.h"
#include "DCPU16InstrInfo.h"
#include "llvm/Constants.h"
#include "llvm/PassSupport.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/MC/MCSymbol.h"
#include <algorithm>
#include <iostream>

using namespace llvm;

static cl::opt<bool> DisableDCPU16Peephole(
  "disable-dcpu16-peephole",
  cl::Hidden,
  cl::ZeroOrMore,
  cl::init(false),
  cl::desc("Disable Peephole Optimisations")
);

static cl::opt<bool> DisableOptBrcc(
  "disable-dcpu16-brcc",
  cl::Hidden,
  cl::ZeroOrMore,
  cl::init(false),
  cl::desc("Disable Conditional Branch Optimization")
);

namespace {
  struct DCPU16Peephole : public MachineFunctionPass {
    const DCPU16InstrInfo     *QII;
    const DCPU16RegisterInfo  *QRI;
    const MachineRegisterInfo *MRI;

  public:
    static char ID;
    DCPU16Peephole() : MachineFunctionPass(ID) { }

    bool runOnMachineFunction(MachineFunction &MF);
    void runOptBrcc(MachineBasicBlock *mbb);
    bool swapOptBrcc(MachineInstr *brInstr, MachineInstr *andInstr);

    const char *getPassName() const {
      return "DCPU16 optimize conditional branches";
    }

    void getAnalysisUsage(AnalysisUsage &AU) const {
      MachineFunctionPass::getAnalysisUsage(AU);
    }
  };
}

char DCPU16Peephole::ID = 0;

bool DCPU16Peephole::swapOptBrcc(MachineInstr *brInstr, MachineInstr *andInstr) {
  MachineOperand &brA = brInstr->getOperand(1);
  MachineOperand &brB = brInstr->getOperand(2);
  MachineOperand &andA = andInstr->getOperand(1);
  MachineOperand &andB = andInstr->getOperand(2);
  
  if(brA.isReg() && andA.isReg() && brB.isReg() && andB.isReg()) {
    brA.ChangeToRegister(
      andA.getReg(),
      andA.isDef(),
      andA.isImplicit(),
      true,
      true,
      andA.isUndef(),
      andA.isDebug()
    );
    
    brB.ChangeToRegister(
      andB.getReg(),
      andB.isDef(),
      andB.isImplicit(),
      andB.isKill(),
      andB.isDead(),
      andB.isUndef(),
      andB.isDebug()
    );
    
    return true;
  } else if(brA.isReg() && andA.isReg() && brB.isImm() && andB.isImm()) {
    brA.ChangeToRegister(
      andA.getReg(),
      andA.isDef(),
      andA.isImplicit(),
      true,
      true,
      andA.isUndef(),
      andA.isDebug()
    );
    
    brB.setImm(andB.getImm());
    
    return true;
  } else {
    assert("Encountered unexpected combination in swapOptBrcc");
    return false;
  }
}

void DCPU16Peephole::runOptBrcc(MachineBasicBlock *mbb) {  
  DenseMap<unsigned, MachineInstr *> peepholeMap;
  
  mbb->dump();
  std::cout << std::endl;
  
  for(MachineBasicBlock::iterator miiIter = mbb->begin(); miiIter != mbb->end(); ++miiIter) {
    MachineInstr *instruction = miiIter;  
    
    switch(instruction->getOpcode()) {
      // And instructions
      case DCPU16::AND16ri: {
        assert(instruction->getNumOperands() == 4);
        
        MachineOperand &result = instruction->getOperand(0);
        unsigned resultReg = result.getReg();
        
        if(TargetRegisterInfo::isVirtualRegister(resultReg))
          peepholeMap[resultReg] = instruction;
        
        break;
      }
      
      // Branch instructions
      case DCPU16::BR_CCri: {
        assert(instruction->getNumOperands() == 4);
        
        if(instruction->getOperand(2).getImm() != 0) break; // Only works if comparing to 0
        
        MachineOperand &activeOperand = instruction->getOperand(1);
        unsigned activeReg = activeOperand.getReg();
        
        if(MachineInstr *peepholeSource = peepholeMap.lookup(activeReg)) {              
          // Change the branch instruction
          if(instruction->getOperand(0).getImm() == DCPU16CC::COND_NE) {
            instruction->getOperand(0).setImm(DCPU16CC::COND_B);
          } else if(instruction->getOperand(0).getImm() == DCPU16CC::COND_E) {
            instruction->getOperand(0).setImm(DCPU16CC::COND_C);
          } else { break; }
          
          swapOptBrcc(instruction, peepholeSource);
          
          // Remove the AND from the block
          peepholeSource->eraseFromParent();
        }
      }
    }
  }
}

bool DCPU16Peephole::runOnMachineFunction(MachineFunction &MF) {
  QII = static_cast<const DCPU16InstrInfo *>(MF.getTarget().getInstrInfo());
  QRI = static_cast<const DCPU16RegisterInfo *>(MF.getTarget().getRegisterInfo());
  MRI = &MF.getRegInfo();
  
  // Disable all peephole optimisations
  if(DisableDCPU16Peephole) return false;

  // Loop over all of the basic blocks.
  for(MachineFunction::iterator mbbIter = MF.begin(); mbbIter != MF.end(); ++mbbIter) {
    MachineBasicBlock *mbb = mbbIter;
    
    if(!DisableOptBrcc) runOptBrcc(mbb);
  } // Basic Block
  return true;
}

FunctionPass *llvm::createDCPU16Peephole() {
  return new DCPU16Peephole();
}

