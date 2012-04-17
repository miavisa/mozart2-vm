// Copyright © 2012, Université catholique de Louvain
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// *  Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// *  Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "mozart.hh"

namespace mozart {

struct SuspendTrailEntry {
  SuspendTrailEntry(StableNode* left, StableNode* right) :
    left(left), right(right) {}

  StableNode* left;
  StableNode* right;
};

typedef VMAllocatedList<SuspendTrailEntry> SuspendTrail;

typedef VMAllocatedList<NodeBackup> RebindTrail;

struct StructuralDualWalk {
public:
  enum Kind {
    wkUnify, wkEquals
  };
public:
  StructuralDualWalk(VM vm, Kind kind) : vm(vm), kind(kind) {}

  OpResult run(RichNode left, RichNode right);
private:
  inline
  OpResult processPair(VM vm, RichNode left, RichNode right);

  inline
  void rebind(VM vm, RichNode left, RichNode right);

  inline
  void undoBindings(VM vm);

  VM vm;
  Kind kind;

  WalkStack stack;
  RebindTrail rebindTrail;
  SuspendTrail suspendTrail;
};

/////////////////
// Entry point //
/////////////////

OpResult fullUnify(VM vm, RichNode left, RichNode right) {
  StructuralDualWalk walk(vm, StructuralDualWalk::wkUnify);
  return walk.run(left, right);
}

OpResult fullEquals(VM vm, RichNode left, RichNode right, bool* result) {
  StructuralDualWalk walk(vm, StructuralDualWalk::wkEquals);
  OpResult res = walk.run(left, right);

  switch (res.kind()) {
    case OpResult::orProceed: {
      *result = true;
      return OpResult::proceed();
    }

    case OpResult::orFail: {
      *result = false;
      return OpResult::proceed();
    }

    default: {
      return res;
    }
  }
}

////////////////////
// The real thing //
////////////////////

OpResult StructuralDualWalk::run(RichNode left, RichNode right) {
  VM vm = this->vm;

  UnstableNode unstableLeft, unstableRight;

  while (true) {
    // Process the pair
    OpResult pairResult = processPair(vm, left, right);

    switch (pairResult.kind()) {
      case OpResult::orFail:
      case OpResult::orRaise: {
        stack.clear(vm);
        suspendTrail.clear(vm);
        undoBindings(vm);
        return pairResult;
      }

      case OpResult::orWaitBefore: {
        suspendTrail.push_back_new(vm, left.getStableRef(vm),
                                   right.getStableRef(vm));
        break;
      }

      case OpResult::orProceed: {
        // nothing to do
      }
    }

    // Finished?
    if (stack.empty())
      break;

    // Pop next pair
    unstableLeft.copy(vm, *stack.front().left);
    unstableRight.copy(vm, *stack.front().right);

    left = unstableLeft;
    right = unstableRight;

    // Go to next item
    stack.remove_front(vm);
  }

  // Do we need to suspend on something?
  if (!suspendTrail.empty()) {
    // Undo temporary bindings

    undoBindings(vm);

    // Create the control variable

    UnstableNode unstableControlVar = Variable::build(vm);
    RichNode controlVar = unstableControlVar;
    controlVar.ensureStable(vm);

    // Reduce the remaining unifications
    size_t count = suspendTrail.size();

    if (count == 1) {
      unstableLeft.copy(vm, *suspendTrail.front().left);
      unstableRight.copy(vm, *suspendTrail.front().right);

      left = unstableLeft;
      right = unstableRight;

      if (left.isTransient())
        DataflowVariable(left).addToSuspendList(vm, controlVar);

      if (right.isTransient())
        DataflowVariable(right).addToSuspendList(vm, controlVar);
    } else {
      UnstableNode label = Atom::build(vm, u"#");

      unstableLeft.make<Tuple>(vm, count, &label);
      unstableRight.make<Tuple>(vm, count, &label);

      auto leftTuple = RichNode(unstableLeft).as<Tuple>();
      auto rightTuple = RichNode(unstableRight).as<Tuple>();

      size_t i = 0;
      for (auto iter = suspendTrail.begin();
           iter != suspendTrail.end(); i++, ++iter) {
        UnstableNode leftTemp(vm, *iter->left);
        leftTuple.initElement(vm, i, &leftTemp);

        RichNode richLeftTemp = leftTemp;
        if (richLeftTemp.isTransient())
          DataflowVariable(richLeftTemp).addToSuspendList(vm, controlVar);

        UnstableNode rightTemp(vm, *iter->right);
        rightTuple.initElement(vm, i, &rightTemp);

        RichNode richRightTemp = rightTemp;
        if (richRightTemp.isTransient())
          DataflowVariable(richRightTemp).addToSuspendList(vm, controlVar);
      }
    }

    suspendTrail.clear(vm);

    // TODO Replace initial operands by unstableLeft and unstableRight

    return OpResult::waitFor(vm, controlVar);
  }

  /* No need to undo temporary bindings here, even if we are in wkEquals mode.
   * In fact, we should not undo them in that case, as that compactifies
   * the store, and speeds up subsequent tests and/or unifications.
   *
   * However, the above is *wrong* when in a subspace, because of speculative
   * bindings.
   */
  if (!vm->isOnTopLevel())
    undoBindings(vm);

  return OpResult::proceed();
}

OpResult StructuralDualWalk::processPair(VM vm, RichNode left,
                                         RichNode right) {
  // Identical nodes
  if (left.isSameNode(right))
    return OpResult::proceed();

  const Type* leftType = left.type();
  const Type* rightType = right.type();

  StructuralBehavior leftBehavior = leftType->getStructuralBehavior();
  StructuralBehavior rightBehavior = rightType->getStructuralBehavior();

  // One of them is a variable
  switch (kind) {
    case wkUnify: {
      if (leftBehavior == sbVariable) {
        if (rightBehavior == sbVariable) {
          if (leftType->getBindingPriority() > rightType->getBindingPriority())
            return DataflowVariable(left).bind(vm, right);
          else
            return DataflowVariable(right).bind(vm, left);
        } else {
          return DataflowVariable(left).bind(vm, right);
        }
      } else if (rightBehavior == sbVariable) {
        return DataflowVariable(right).bind(vm, left);
      }

      break;
    }

    case wkEquals: {
      if (leftBehavior == sbVariable) {
        assert(leftType->isTransient());
        return OpResult::waitFor(vm, left);
      } else if (rightBehavior == sbVariable) {
        assert(rightType->isTransient());
        return OpResult::waitFor(vm, right);
      }

      break;
    }
  }

  // If we reach this, both left and right are non-var
  if (leftType != rightType)
    return OpResult::fail();

  switch (leftBehavior) {
    case sbValue: {
      bool success = ValueEquatable(left).equals(vm, right);
      return success ? OpResult::proceed() : OpResult::fail();
    }

    case sbStructural: {
      bool success = StructuralEquatable(left).equals(vm, right, stack);
      if (success) {
        rebind(vm, left, right);
        return OpResult::proceed();
      } else {
        return OpResult::fail();
      }
    }

    case sbTokenEq: {
      assert(!left.isSameNode(right)); // this was tested earlier
      return OpResult::fail();
    }

    case sbVariable: {
      assert(false);
      return OpResult::fail();
    }
  }

  // We should not reach this point
  assert(false);
  return OpResult::proceed();
}

void StructuralDualWalk::rebind(VM vm, RichNode left, RichNode right) {
  rebindTrail.push_back(vm, left.makeBackup());
  left.reinit(vm, right);
}

void StructuralDualWalk::undoBindings(VM vm) {
  while (!rebindTrail.empty()) {
    rebindTrail.front().restore();
    rebindTrail.remove_front(vm);
  }
}

}
