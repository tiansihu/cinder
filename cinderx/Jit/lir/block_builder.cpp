// Copyright (c) Meta Platforms, Inc. and affiliates. (http://www.meta.com)

#include "cinderx/Jit/lir/block_builder.h"

#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/util.h"

#include <dlfcn.h>

#include <sstream>

// XXX: this file needs to be revisited when we optimize HIR-to-LIR translation
// in codegen.cpp/h. Currently, this file is almost an identical copy from
// bbbuilder.cpp with some interfaces changes so that it works with the new
// LIR.

namespace jit::lir {

BasicBlockBuilder::BasicBlockBuilder(jit::codegen::Environ* env, Function* func)
    : env_(env), func_(func) {}

std::size_t BasicBlockBuilder::makeDeoptMetadata() {
  JIT_CHECK(
      cur_hir_instr_ != nullptr,
      "Can't make DeoptMetadata with a nullptr HIR instruction");
  auto deopt_base = cur_hir_instr_->asDeoptBase();
  JIT_CHECK(deopt_base != nullptr, "Current HIR instruction can't deopt");

  if (!cur_deopt_metadata_.has_value()) {
    cur_deopt_metadata_ = env_->rt->addDeoptMetadata(
        DeoptMetadata::fromInstr(*deopt_base, env_->code_rt));
  }
  return cur_deopt_metadata_.value();
}

BasicBlock* BasicBlockBuilder::allocateBlock(std::string_view label) {
  auto [it, inserted] = label_to_bb_.emplace(std::string{label}, nullptr);
  if (inserted) {
    it->second = func_->allocateBasicBlock();
  }
  return it->second;
}

void BasicBlockBuilder::appendBlock(BasicBlock* block) {
  if (cur_bb_->successors().size() < 2) {
    cur_bb_->addSuccessor(block);
  }
  switchBlock(block);
}

void BasicBlockBuilder::switchBlock(BasicBlock* block) {
  bbs_.push_back(block);
  cur_bb_ = block;
}

void BasicBlockBuilder::appendLabel(std::string_view s) {
  appendBlock(allocateBlock(s));
}

Instruction* BasicBlockBuilder::createInstr(Instruction::Opcode opcode) {
  return cur_bb_->allocateInstr(opcode, cur_hir_instr_);
}

BasicBlock* BasicBlockBuilder::getBasicBlockByLabel(const std::string& label) {
  auto iter = label_to_bb_.find(label);
  if (iter == label_to_bb_.end()) {
    auto bb = func_->allocateBasicBlock();
    label_to_bb_.emplace(label, bb);
    return bb;
  }

  return iter->second;
}

Instruction* BasicBlockBuilder::getDefInstr(const hir::Register* reg) {
  auto def_instr = map_get(env_->output_map, reg, nullptr);

  if (def_instr == nullptr) {
    // The output has to be copy propagated.
    hir::Register* def_reg = nullptr;
    auto iter = env_->copy_propagation_map.find(reg);
    while (iter != env_->copy_propagation_map.end()) {
      def_reg = iter->second;
      iter = env_->copy_propagation_map.find(def_reg);
    }

    if (def_reg != nullptr) {
      def_instr = map_get(env_->output_map, def_reg, nullptr);
    }
  }

  return def_instr;
}

void BasicBlockBuilder::createInstrInput(
    Instruction* instr,
    hir::Register* reg) {
  instr->allocateLinkedInput(getDefInstr(reg));
}

void BasicBlockBuilder::createInstrOutput(
    Instruction* instr,
    hir::Register* dst) {
  auto pair = env_->output_map.emplace(dst, instr);
  JIT_DCHECK(
      pair.second,
      "Multiple outputs with the same name ({})- HIR is not in SSA form.",
      dst->name());
  auto output = instr->output();
  output->setVirtualRegister();
  output->setDataType(hirTypeToDataType(dst->type()));
}

void BasicBlockBuilder::SetBlockSection(
    const std::string& label,
    codegen::CodeSection section) {
  BasicBlock* block = getBasicBlockByLabel(label);
  if (block == nullptr) {
    return;
  }
  block->setSection(section);
}

} // namespace jit::lir
