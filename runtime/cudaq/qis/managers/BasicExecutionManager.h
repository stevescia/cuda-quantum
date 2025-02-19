/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

#include "common/ExecutionContext.h"
#include "common/Logger.h"
#include "cudaq/qis/execution_manager.h"

#include <complex>
#include <functional>
#include <map>
#include <queue>
#include <stack>

namespace cudaq {

/// @brief The BasicExecutionManager provides a common base class for
/// specializations that implements the ExecutionManager type. Most of the
/// required ExecutionManager functionality is implemented here, with
/// backend-execution-specific details left for further subtypes. This type
/// enqueues all quantum operations and flush them at specific synchronization
/// points. Subtypes should implement concrete operation execution, qudit
/// measurement, allocation, and deallocation, and execution context handling
/// (e.g. sampling)
class BasicExecutionManager : public cudaq::ExecutionManager {
protected:
  /// @brief An instruction is composed of a operation name,
  /// a optional set of rotation parameters, control qudits, and
  /// target qudits.
  using Instruction =
      std::tuple<std::string, std::vector<double>,
                 std::vector<cudaq::QuditInfo>, std::vector<cudaq::QuditInfo>>;

  /// @brief Typedef for a queue of instructions
  using InstructionQueue = std::queue<Instruction>;

  /// @brief The current execution context, e.g. sampling
  /// or observation
  cudaq::ExecutionContext *executionContext;

  /// @brief Store qubits for delayed deletion under
  /// certain execution contexts
  std::vector<std::size_t> contextQuditIdsForDeletion;

  /// @brief The current queue of operations to execute
  InstructionQueue instructionQueue;

  /// @brief When adjoint operations are requested
  /// we can store them here for delayed execution
  std::stack<InstructionQueue> adjointQueueStack;

  /// @brief When we are in a control region, we
  /// need to store extra control qudit ids.
  std::vector<std::size_t> extraControlIds;

  /// @brief Flag to indicate we are in an adjoint region
  bool inAdjointRegion = false;

  /// @brief Subtype-specific qudit allocation method
  virtual void allocateQudit(const QuditInfo &q) = 0;

  /// @brief Subtype-specific qudit deallocation method
  virtual void deallocateQudit(std::size_t q) = 0;

  /// @brief Subtype-specific handler for when
  // the execution context changes
  virtual void handleExecutionContextChanged() = 0;

  /// @brief Subtype-specific handler for when the
  /// current execution context has ended.
  virtual void handleExecutionContextEnded() = 0;

  /// @brief Subtype-specific method for affecting the
  /// execution of a queued instruction.
  virtual void executeInstruction(const Instruction &inst) = 0;

  /// @brief Subtype-specific method for performing qudit measurement.
  virtual int measureQudit(const cudaq::QuditInfo &q) = 0;

public:
  BasicExecutionManager() = default;
  virtual ~BasicExecutionManager() = default;

  void setExecutionContext(cudaq::ExecutionContext *_ctx) override {
    executionContext = _ctx;
    handleExecutionContextChanged();
    while (!instructionQueue.empty()) {
      instructionQueue.pop();
    }
  }

  void resetExecutionContext() override {
    synchronize();
    std::string_view ctx_name = "";
    if (executionContext)
      ctx_name = executionContext->name;

    // Do any final post-processing before
    // we deallocate the qudits
    handleExecutionContextEnded();

    if (ctx_name == "observe" || ctx_name == "sample" ||
        ctx_name == "extract-state") {
      for (auto &q : contextQuditIdsForDeletion) {
        deallocateQudit(q);
        returnIndex(q);
      }
      contextQuditIdsForDeletion.clear();
    }
    executionContext = nullptr;
  }

  std::size_t getAvailableIndex(std::size_t quditLevels) override {
    auto new_id = getNextIndex();
    allocateQudit({quditLevels, new_id});
    return new_id;
  }

  void returnQudit(const QuditInfo &qid) override {
    if (!executionContext) {
      deallocateQudit(qid.id);
      returnIndex(qid.id);
      return;
    }

    std::string_view ctx_name = "";
    if (executionContext)
      ctx_name = executionContext->name;

    // Handle the case where we are sampling with an implicit
    // measure on the entire register.
    if (executionContext && (ctx_name == "observe" || ctx_name == "sample" ||
                             ctx_name == "extract-state")) {
      contextQuditIdsForDeletion.push_back(qid.id);
      return;
    }

    deallocateQudit(qid.id);
    returnIndex(qid.id);
    if (numAvailable() == totalNumQudits()) {
      if (executionContext && ctx_name == "observe") {
        while (!instructionQueue.empty())
          instructionQueue.pop();
      }
    }
  }

  void startAdjointRegion() override { adjointQueueStack.emplace(); }

  void endAdjointRegion() override {
    // Get the top queue and remove it
    auto adjointQueue = adjointQueueStack.top();
    adjointQueueStack.pop();

    // Reverse it
    [](InstructionQueue &q) {
      std::stack<Instruction> s;
      while (!q.empty()) {
        s.push(q.front());
        q.pop();
      }
      while (!s.empty()) {
        q.push(s.top());
        s.pop();
      }
    }(adjointQueue);

    while (!adjointQueue.empty()) {
      auto front = adjointQueue.front();
      adjointQueue.pop();
      if (adjointQueueStack.empty()) {
        instructionQueue.push(front);
      } else {
        adjointQueueStack.top().push(front);
      }
    }
  }

  void startCtrlRegion(const std::vector<std::size_t> &controls) override {
    for (auto &c : controls)
      extraControlIds.push_back(c);
  }

  void endCtrlRegion(const std::size_t n_controls) override {
    // extra_control_qubits.erase(end - n_controls, end);
    extraControlIds.resize(extraControlIds.size() - n_controls);
  }

  /// The goal for apply is to create a new element of the
  /// instruction queue (a tuple).
  void apply(const std::string_view gateName,
             const std::vector<double> &&params,
             const std::vector<cudaq::QuditInfo> &controls,
             const std::vector<cudaq::QuditInfo> &targets,
             bool isAdjoint = false) override {

    // Make a copy of the name that we can mutate if necessary
    std::string mutable_name(gateName);

    // Make a copy of the params that we can mutate
    std::vector<double> mutable_params = params;

    // Create an array of controls, we will
    // prepend any extra controls if in a control region
    std::vector<cudaq::QuditInfo> mutable_controls;
    for (auto &e : extraControlIds) {
      mutable_controls.emplace_back(2, e);
    }
    for (auto &e : controls) {
      mutable_controls.push_back(e);
    }

    std::vector<cudaq::QuditInfo> mutable_targets;
    for (auto &t : targets) {
      mutable_targets.push_back(t);
    }

    if (isAdjoint || !adjointQueueStack.empty()) {
      for (std::size_t i = 0; i < params.size(); i++) {
        mutable_params[i] = -1.0 * params[i];
      }
      if (mutable_name == "t") {
        mutable_name = "tdg";
      } else if (mutable_name == "s") {
        mutable_name = "sdg";
      }
    }

    if (!adjointQueueStack.empty()) {
      // Add to the adjoint instruction queue
      adjointQueueStack.top().emplace(std::make_tuple(
          mutable_name, mutable_params, mutable_controls, mutable_targets));
    } else {
      // Add to the instruction queue
      instructionQueue.emplace(std::make_tuple(std::move(mutable_name),
                                               mutable_params, mutable_controls,
                                               mutable_targets));
    }
  }

  void synchronize() override {
    while (!instructionQueue.empty()) {
      auto instruction = instructionQueue.front();
      executeInstruction(instruction);
      instructionQueue.pop();
    }
  }

  int measure(const cudaq::QuditInfo &target) override {
    // We hit a measure, need to exec / clear instruction queue
    synchronize();

    // Instruction executed, run the measure call
    return measureQudit(target);
  }
};

} // namespace cudaq
