// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include "Jit/codegen/inliner.h"
#include <dlfcn.h>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include "Jit/lir/lir.h"
#include "Jit/lir/parser.h"

using namespace jit::lir;

namespace jit {
namespace codegen {

bool LIRInliner::inlineCall() {
  // Try to find function.
  Function* callee = findFunction();
  if (callee == nullptr) {
    // If function is not found, we cannot inline.
    JIT_DLOG(
        "Cannot find the function that corresponds to the call instruction.");
    return false;
  }

  if (!isInlineable(callee)) {
    JIT_DLOG("Found the callee, but cannot inline.");
    return false;
  }

  // Split basic blocks of caller.
  BasicBlock* block1 = call_instr_->basicblock();
  BasicBlock* block2 = block1->splitBefore(call_instr_);

  // Copy callee into caller.
  Function* caller = call_instr_->basicblock()->function();
  Function::CopyResult callee_bounds = caller->copyFrom(callee, block1, block2);
  callee_start_ = callee_bounds.begin_bb;
  callee_end_ = callee_bounds.end_bb;

  resolveArguments();

  resolveReturnValue();

  return true;
}

bool LIRInliner::isInlineable(Function* callee) {
  if (!checkEntryExitReturn(callee)) {
    return false;
  }
  if (!checkArguments()) {
    return false;
  }
  if (!checkLoadArg(callee)) {
    return false;
  }
  return true;
}

bool LIRInliner::checkEntryExitReturn(Function* callee) {
  if (callee->basicblocks().empty()) {
    JIT_DLOG("Callee has no basic block.");
    return false;
  }
  BasicBlock* entry_block = callee->getEntryBlock();
  if (!entry_block->predecessors().empty()) {
    JIT_DLOG("Expect entry block to have no predecessors.");
    return false;
  }
  BasicBlock* exit_block = callee->basicblocks().back();
  if (!exit_block->successors().empty()) {
    JIT_DLOG("Expect exit block to have no successors.");
    return false;
  }
  for (BasicBlock* bb : callee->basicblocks()) {
    if (bb->predecessors().empty() && bb != entry_block) {
      JIT_DLOG("Expect callee to have only 1 entry block.");
      return false;
    }
    if (bb->successors().empty() && bb != exit_block) {
      JIT_DLOG("Expect callee to have only 1 exit block.");
      return false;
    }
    for (auto& instr : bb->instructions()) {
      if (instr->isReturn()) {
        if (instr.get() != bb->getLastInstr() || bb->successors().size() != 1 ||
            bb->successors()[0] != exit_block) {
          JIT_DLOG(
              "Expect return to be last instruction of the predecessor of the "
              "exit block.");
          // Expect return to be the last instruction in the block.
          // Expect the successor to be the exit block.
          return false;
        }
      }
    }
  }
  if (!exit_block->instructions().empty()) {
    JIT_DLOG("Expect exit block to have no instructions.");
    return false;
  }
  return true;
}

bool LIRInliner::checkArguments() {
  size_t numInputs = call_instr_->getNumInputs();
  for (size_t i = 1; i < numInputs; ++i) {
    auto input = call_instr_->getInput(i);
    if (!input->isImm() && !input->isVreg()) {
      return false;
    }
    arguments_.emplace_back(input);
  }
  return true;
}

bool LIRInliner::checkLoadArg(Function* callee) {
  // Subtract by 1 since first argument is callee address.
  size_t numInputs = call_instr_->getNumInputs() - 1;
  // Use check_load_arg to track if we are still in LoadArg instructions.
  bool check_load_arg = true;
  for (auto bb : callee->basicblocks()) {
    for (auto& instr : bb->instructions()) {
      if (check_load_arg) {
        if (instr->isLoadArg()) {
          if (instr->getNumInputs() < 1) {
            return false;
          }
          auto input = instr->getInput(0);
          if (!input->isImm()) {
            return false;
          }
          if (input->getConstant() >= numInputs) {
            return false;
          }
        } else {
          // No longer LoadArg instructions.
          check_load_arg = false;
        }
      } else {
        if (instr->isLoadArg()) {
          // kLoadArg instructions should only be at the
          // beginning of the callee.
          return false;
        }
      }
    }
  }
  return true;
}

lir::Function* LIRInliner::findFunction() {
  // Get the address.
  if (call_instr_->getNumInputs() < 1) {
    return nullptr;
  }
  OperandBase* dest_operand = call_instr_->getInput(0);
  if (!dest_operand->isImm()) {
    return nullptr;
  }
  uint64_t addr = dest_operand->getConstant();

  // Resolve the addres to a name.
  Dl_info helper_info;
  if (dladdr(reinterpret_cast<void*>(addr), &helper_info) == 0 ||
      helper_info.dli_sname == NULL) {
    return nullptr;
  }
  const std::string& name = helper_info.dli_sname;
  return parseFunction(name);
}

lir::Function* LIRInliner::parseFunction(const std::string& name) {
  static std::unordered_map<std::string, std::unique_ptr<Function>>
      name_to_function;

  // Check if function is in map, return function if in map.
  auto iter = name_to_function.find(name);
  if (iter != name_to_function.end()) {
    return iter->second.get();
  }

  // Using function name, try to open and parse text file with function.
  std::ifstream lir_text(
      fmt::format("Jit/lir/c_helper_translations/{}.lir", name));
  if (!lir_text.good()) {
    return nullptr;
  }
  std::stringstream buffer;
  buffer << lir_text.rdbuf();
  Parser parser;
  std::unique_ptr<Function> parsed_func = parser.parse(buffer.str());
  // Add function to map.
  name_to_function.emplace(name, std::move(parsed_func));
  // Return parsed function.
  return map_get_strict(name_to_function, name).get();
}

bool LIRInliner::resolveArguments() {
  // Remove load arg instructions and update virtual registers.
  std::unordered_map<OperandBase*, LinkedOperand*> vreg_map;
  auto caller_blocks = &call_instr_->basicblock()->function()->basicblocks();
  for (int i = callee_start_; i < callee_end_; i++) {
    auto bb = caller_blocks->at(i);
    auto it = bb->instructions().begin();
    // Use while loop since instructions may be removed.
    while (it != bb->instructions().end()) {
      if ((*it)->isLoadArg()) {
        resolveLoadArg(vreg_map, bb, it);
      } else {
        // When instruction is not kLoadArg,
        // fix any inputs are linked to output registers from kLoadArg.
        resolveLinkedArgumentsUses(vreg_map, it);
      }
    }
  }

  return true;
}

void LIRInliner::resolveLoadArg(
    std::unordered_map<OperandBase*, LinkedOperand*>& vreg_map,
    BasicBlock* bb,
    BasicBlock::InstrList::iterator& instr_it) {
  auto instr = instr_it->get();
  JIT_DCHECK(
      instr->getNumInputs() > 0 && instr->getInput(0)->isImm(),
      "LoadArg instruction should have at least 1 input.");

  // Get the corresponding parameter from the call instruction.
  auto argument = instr->getInput(0);
  auto param = arguments_.at(argument->getConstant());

  // Based on the parameter type, resolve the kLoadArg.
  if (param->isImm()) {
    // For immediate values, change kLoadArg to kMove.
    instr->setOpcode(Instruction::kMove);
    auto param_copy =
        std::make_unique<Operand>(instr, static_cast<Operand*>(param));
    param_copy->setConstant(param->getConstant());
    instr->replaceInputOperand(0, std::move(param_copy));
    ++instr_it;
  } else {
    JIT_DCHECK(
        param->isLinked(), "Inlined arguments must be immediate or linked.");
    // Otherwise, output of kLoadArg should be a virtual register.
    // For virtual registers, delete kLoadArg and replace uses.
    vreg_map.emplace(instr->output(), static_cast<LinkedOperand*>(param));
    instr_it = bb->instructions().erase(instr_it);
  }
}

void LIRInliner::resolveLinkedArgumentsUses(
    std::unordered_map<OperandBase*, LinkedOperand*>& vreg_map,
    std::list<std::unique_ptr<Instruction>>::iterator& instr_it) {
  auto setLinkedOperand = [&](OperandBase* opnd) {
    auto new_def = map_get(vreg_map, opnd->getDefine(), nullptr);
    if (new_def != nullptr) {
      auto opnd_linked = static_cast<LinkedOperand*>(opnd);
      opnd_linked->setLinkedInstr(new_def->getLinkedOperand()->instr());
    }
  };
  auto instr = instr_it->get();
  for (size_t i = 0, n = instr->getNumInputs(); i < n; i++) {
    auto input = instr->getInput(i);
    if (input->isLinked()) {
      setLinkedOperand(input);
    } else if (input->isInd()) {
      // For indirect operands, check if base or index registers are linked.
      auto memInd = input->getMemoryIndirect();
      auto base = memInd->getBaseRegOperand();
      auto index = memInd->getIndexRegOperand();
      if (base->isLinked()) {
        setLinkedOperand(base);
      }
      if (index && index->isLinked()) {
        setLinkedOperand(index);
      }
    }
  }
  ++instr_it;
}

void LIRInliner::resolveReturnValue() {
  auto epilogue =
      call_instr_->basicblock()->function()->basicblocks().at(callee_end_ - 1);

  // Create phi instruction.
  auto phi_instr =
      epilogue->allocateInstr(Instruction::kPhi, nullptr, OutVReg());

  // Find return instructions from predecessor of epilogue.
  for (auto pred : epilogue->predecessors()) {
    auto lastInstr = pred->getLastInstr();
    if (lastInstr != nullptr && lastInstr->isReturn()) {
      phi_instr->allocateLabelInput(pred);
      JIT_CHECK(
          lastInstr->getNumInputs() > 0,
          "Return instruction should have at least 1 input operand.");
      phi_instr->appendInputOperand(lastInstr->releaseInputOperand(0));
      pred->removeInstr(pred->getLastInstrIter());
    }
  }

  if (phi_instr->getNumInputs() == 0) {
    // Callee has no return statements.
    // Remove phi instruction.
    epilogue->removeInstr(epilogue->getLastInstrIter());
    call_instr_->setOpcode(Instruction::kNop);
  } else {
    call_instr_->setOpcode(Instruction::kMove);
    // Remove all inputs.
    while (call_instr_->getNumInputs() > 0) {
      call_instr_->removeInputOperand(0);
    }
    call_instr_->allocateLinkedInput(phi_instr);
  }
}

} // namespace codegen
} // namespace jit