//===--- SILCombinerCastVisitors.cpp --------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-combine"
#include "SILCombiner.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;
using namespace swift::PatternMatch;

SILInstruction *
SILCombiner::visitRefToRawPointerInst(RefToRawPointerInst *rrpi) {
  if (auto *urci = dyn_cast<UncheckedRefCastInst>(rrpi->getOperand())) {
    // In this optimization, we try to move ref_to_raw_pointer up the def-use
    // graph. E.x.:
    //
    // ```
    // %0 = ...
    // %1 = unchecked_ref_cast %0
    // %2 = ref_to_raw_pointer %1
    // ```
    //
    // to:
    //
    // ```
    // %0 = ...
    // %2 = ref_to_raw_pointer %0
    // %1 = unchecked_ref_cast %0
    // ```
    //
    // If we find that the unchecked_ref_cast has no uses, we then eliminate
    // it.
    //
    // Naturally, this requires us to always hoist our new instruction (or
    // modified instruction) to before the unchecked_ref_cast.
    //
    // First we handle the case where we have a class type where we do not need
    // to insert a new instruction.
    if (urci->getOperand()->getType().isAnyClassReferenceType()) {
      rrpi->setOperand(urci->getOperand());
      rrpi->moveBefore(urci);
      return urci->use_empty() ? eraseInstFromFunction(*urci) : nullptr;
    }

    // Otherwise, we ened to use an unchecked_trivial_bit_cast insert it at
    // urci.
    //
    // (ref_to_raw_pointer (unchecked_ref_cast x))
    //    -> (unchecked_trivial_bit_cast x)
    auto *utbi = withBuilder(urci, [&](auto &b, auto l) {
      return b.createUncheckedTrivialBitCast(l, urci->getOperand(),
                                             rrpi->getType());
    });
    rrpi->replaceAllUsesWith(utbi);
    eraseInstFromFunction(*rrpi);
    return urci->use_empty() ? eraseInstFromFunction(*urci) : nullptr;
  }

  // (ref_to_raw_pointer (open_existential_ref (init_existential_ref x))) ->
  // (ref_to_raw_pointer x)
  //
  // In terms of ownership, we need to insert this at the init_existential to
  // ensure that x is live if we have an owned value.
  if (auto *oeri = dyn_cast<OpenExistentialRefInst>(rrpi->getOperand())) {
    if (auto *ieri = dyn_cast<InitExistentialRefInst>(oeri->getOperand())) {
      auto *utbi = withBuilder(ieri, [&](auto &b, auto l) {
        return b.createRefToRawPointer(l, ieri->getOperand(), rrpi->getType());
      });
      rrpi->replaceAllUsesWith(utbi);
      return eraseInstFromFunction(*rrpi);
    }
  }

  return nullptr;
}

namespace {

/// A folder object for sequences of forwarding instructions that forward owned
/// ownership. Is used to detect if we can delete the intermediate forwarding
/// instructions without ownership issues and then allows the user to either
/// delete all of the rest of the forwarding instructions and then replace front
/// with a new value or set front's operand to a new value.
class SingleBlockOwnedForwardingInstFolder {
  SmallVector<SingleValueInstruction *, 4> rest;
  SILCombiner &SC;
  SingleValueInstruction *front;

public:
  SingleBlockOwnedForwardingInstFolder(
      SILCombiner &SC, SingleValueInstruction *instructionToFold)
      : SC(SC), front(instructionToFold) {
    // If our initial instruction to fold isn't owned, set it to nullptr to
    // indicate invalid.
    if (SILValue(instructionToFold).getOwnershipKind() != OwnershipKind::Owned)
      front = nullptr;
  }

  bool isValid() const { return bool(front); }

  bool add(SingleValueInstruction *next) {
    assert(isValid());
    if (SILValue(next).getOwnershipKind() != OwnershipKind::Owned)
      return false;

    if (next->getSingleUse()) {
      rest.push_back(next);
      return true;
    }

    if (front->getParent() != next->getParent()) {
      return false;
    }

    // Otherwise, since the two values are in the same block and we want to
    // optimize only if our original value doesn't have any non-debug uses, we
    // know that our value can only have a single non-debug use, the consuming
    // user. So if we are not in that situation, bail.
    if (!hasOneNonDebugUse(next))
      return false;

    rest.push_back(next);
    return true;
  }

  /// Delete all forwarding uses and then RAUW front with newValue.
  SingleValueInstruction *optimizeWithReplacement(SILValue newValue) && {
    // NOTE: Even though after running cleanup rest, front now has its
    // forwarding operand set to Undef, we haven't touched its result. So it is
    // safe to RAUW.
    cleanupRest();
    SC.replaceValueUsesWith(front, newValue);
    return nullptr;
  }

  /// Delete all forwarding uses and then set front's first operand to be \p
  /// newValue.
  SingleValueInstruction *optimizeWithSetValue(SILValue newValue) && {
    cleanupRest();
    assert(isa<SILUndef>(front->getOperand(0)));
    front->setOperand(0, newValue);
    SC.setUseValue(&front->getOperandRef(0), newValue);
    return nullptr;
  }

private:
  /// From backwards -> forwards, for each instruction in rest, delete all of
  /// its debug uses and then set its single remaining use to be SILUndef.
  ///
  /// This means that after this runs front's forwarding operand is now
  /// SILUndef.
  void cleanupRest() & {
    // We process backwards -> forwards. This cleans up everything but the front
    // value.
    while (!rest.empty()) {
      auto *inst = rest.pop_back_val();
      deleteAllDebugUses(inst, SC.getInstModCallbacks());
      auto *next = inst->getSingleUse();
      assert(next);
      assert(rest.empty() || bool(next->getUser() == rest.back()));
      next->set(SILUndef::get(next->get()->getType(), inst->getModule()));
      SC.eraseInstFromFunction(*inst);
    }
  }
};

} // namespace

SILInstruction *SILCombiner::visitUpcastInst(UpcastInst *uci) {
  auto operand = uci->getOperand();

  // %operandUpcast = upcast %0 : $X->Y
  // %upcastInst = upcast %operandUpcast : $Y->Z
  //
  // %operandUpcast = upcast %0 : $X->Y
  // %1 = upcast %0 : $X->Z
  //
  // If operandUpcast does not have any further uses, we delete it.
  if (auto *operandAsUpcast = dyn_cast<UpcastInst>(operand)) {
    if (operand.getOwnershipKind() != OwnershipKind::Owned) {
      uci->setOperand(operandAsUpcast->getOperand());
      return operandAsUpcast->use_empty()
                 ? eraseInstFromFunction(*operandAsUpcast)
                 : nullptr;
    }
    SingleBlockOwnedForwardingInstFolder folder(*this, uci);
    if (folder.add(operandAsUpcast)) {
      return std::move(folder).optimizeWithSetValue(
          operandAsUpcast->getOperand());
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  auto *F = PTAI->getFunction();

  Builder.setCurrentDebugScope(PTAI->getDebugScope());

  // If we reach this point, we know that the types must be different since
  // otherwise simplifyInstruction would have handled the identity case. This is
  // always legal to do since address-to-pointer pointer-to-address implies
  // layout compatibility.
  //
  // (pointer-to-address strict (address-to-pointer %x))
  // -> (unchecked_addr_cast %x)
  if (PTAI->isStrict()) {
    // We can not perform this optimization with ownership until we are able to
    // handle issues around interior pointers and expanding borrow scopes.
    if (!F->hasOwnership()) {
      if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand())) {
        return Builder.createUncheckedAddrCast(PTAI->getLoc(), ATPI->getOperand(),
                                               PTAI->getType());
      }
    }
  }

  // The rest of these canonicalizations optimize the code around
  // pointer_to_address by leave in a pointer_to_address meaning that we do not
  // need to worry about moving addresses out of interior pointer scopes.

  // Turn this also into an index_addr. We generate this pattern after switching
  // the Word type to an explicit Int32 or Int64 in the stdlib.
  //
  // %101 = builtin "strideof"<Int>(%84 : $@thick Int.Type) :
  //         $Builtin.Word
  // %102 = builtin "zextOrBitCast_Word_Int64"(%101 : $Builtin.Word) :
  //         $Builtin.Int64
  // %111 = builtin "smul_with_overflow_Int64"(%108 : $Builtin.Int64,
  //                               %102 : $Builtin.Int64, %20 : $Builtin.Int1) :
  //         $(Builtin.Int64, Builtin.Int1)
  // %112 = tuple_extract %111 : $(Builtin.Int64, Builtin.Int1), 0
  // %113 = builtin "truncOrBitCast_Int64_Word"(%112 : $Builtin.Int64) :
  //         $Builtin.Word
  // %114 = index_raw_pointer %100 : $Builtin.RawPointer, %113 : $Builtin.Word
  // %115 = pointer_to_address %114 : $Builtin.RawPointer to [strict] $*Int
  //
  // This is safe for ownership since our final SIL still has a
  // pointer_to_address meaning that we do not need to worry about interior
  // pointers.
  SILValue Distance;
  SILValue TruncOrBitCast;
  MetatypeInst *Metatype;
  IndexRawPointerInst *IndexRawPtr;
  BuiltinInst *StrideMul;
  if (match(
          PTAI->getOperand(),
          m_IndexRawPointerInst(IndexRawPtr))) {
    SILValue Ptr = IndexRawPtr->getOperand(0);
    SILValue TruncOrBitCast = IndexRawPtr->getOperand(1);
    if (match(TruncOrBitCast, m_ApplyInst(BuiltinValueKind::TruncOrBitCast,
                                          m_TupleExtractOperation(
                                              m_BuiltinInst(StrideMul), 0)))) {
      if (match(StrideMul,
                m_ApplyInst(
                    BuiltinValueKind::SMulOver, m_SILValue(Distance),
                    m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                m_ApplyInst(BuiltinValueKind::Strideof,
                                            m_MetatypeInst(Metatype))))) ||
          match(StrideMul,
                m_ApplyInst(
                    BuiltinValueKind::SMulOver,
                    m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                m_ApplyInst(BuiltinValueKind::Strideof,
                                            m_MetatypeInst(Metatype))),
                    m_SILValue(Distance)))) {

        SILType InstanceType =
            F->getLoweredType(Metatype->getType()
                .castTo<MetatypeType>().getInstanceType());

        auto *Trunc = cast<BuiltinInst>(TruncOrBitCast);

        // Make sure that the type of the metatype matches the type that we are
        // casting to so we stride by the correct amount.
        if (InstanceType.getAddressType() != PTAI->getType()) {
          return nullptr;
        }

        auto *NewPTAI = Builder.createPointerToAddress(PTAI->getLoc(), Ptr,
                                                       PTAI->getType(),
                                                       PTAI->isStrict(),
                                                       PTAI->isInvariant());
        auto DistanceAsWord = Builder.createBuiltin(
            PTAI->getLoc(), Trunc->getName(), Trunc->getType(), {}, Distance);

        return Builder.createIndexAddr(PTAI->getLoc(), NewPTAI, DistanceAsWord);
      }
    }
  }

  // Turn:
  //
  //   %stride = Builtin.strideof(T) * %distance
  //   %ptr' = index_raw_pointer %ptr, %stride
  //   %result = pointer_to_address %ptr, [strict] $T'
  //
  // To:
  //
  //   %addr = pointer_to_address %ptr, [strict] $T
  //   %result = index_addr %addr, %distance
  //
  // This is safe for ownership since our final SIL still has a
  // pointer_to_address meaning that we do not need to worry about interior
  // pointers.
  BuiltinInst *Bytes = nullptr;
  if (match(PTAI->getOperand(),
            m_IndexRawPointerInst(
                m_ValueBase(),
                m_TupleExtractOperation(m_BuiltinInst(Bytes), 0)))) {
    assert(Bytes != nullptr &&
           "Bytes should have been assigned a non-null value");
    if (match(Bytes, m_ApplyInst(BuiltinValueKind::SMulOver, m_ValueBase(),
                                 m_ApplyInst(BuiltinValueKind::Strideof,
                                             m_MetatypeInst(Metatype)),
                                 m_ValueBase()))) {

      SILType InstanceType =
          F->getLoweredType(Metatype->getType()
              .castTo<MetatypeType>().getInstanceType());

      // Make sure that the type of the metatype matches the type that we are
      // casting to so we stride by the correct amount.
      if (InstanceType.getAddressType() != PTAI->getType())
        return nullptr;

      auto IRPI = cast<IndexRawPointerInst>(PTAI->getOperand());
      SILValue Ptr = IRPI->getOperand(0);
      SILValue Distance = Bytes->getArguments()[0];
      auto *NewPTAI =
        Builder.createPointerToAddress(PTAI->getLoc(), Ptr, PTAI->getType(),
                                       PTAI->isStrict(), PTAI->isInvariant());
      return Builder.createIndexAddr(PTAI->getLoc(), NewPTAI, Distance);
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedAddrCastInst(UncheckedAddrCastInst *UADCI) {
  // These are always safe to perform due to interior pointer ownership
  // requirements being transitive along addresses.

  Builder.setCurrentDebugScope(UADCI->getDebugScope());

  // (unchecked_addr_cast (unchecked_addr_cast x X->Y) Y->Z)
  //   ->
  // (unchecked_addr_cast x X->Z)
  if (auto *OtherUADCI = dyn_cast<UncheckedAddrCastInst>(UADCI->getOperand()))
    return Builder.createUncheckedAddrCast(UADCI->getLoc(),
                                           OtherUADCI->getOperand(),
                                           UADCI->getType());

  // (unchecked_addr_cast cls->superclass) -> (upcast cls->superclass)
  if (UADCI->getType() != UADCI->getOperand()->getType() &&
      UADCI->getType().isExactSuperclassOf(UADCI->getOperand()->getType()))
    return Builder.createUpcast(UADCI->getLoc(), UADCI->getOperand(),
                                UADCI->getType());

  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedRefCastInst(UncheckedRefCastInst *urci) {
  // %0 = unchecked_ref_cast %x : $X->Y
  // %1 = unchecked_ref_cast %0 : $Y->Z
  //
  // ->
  //
  // %0 = unchecked_ref_cast %x : $X->Y
  // %1 = unchecked_ref_cast %x : $X->Z
  //
  // NOTE: For owned values, we only perform this optimization if we can
  // guarantee that we can eliminate the initial unchecked_ref_cast.
  if (auto *otherURCI = dyn_cast<UncheckedRefCastInst>(urci->getOperand())) {
    SILValue otherURCIOp = otherURCI->getOperand();
    if (otherURCIOp.getOwnershipKind() != OwnershipKind::Owned) {
      return Builder.createUncheckedRefCast(urci->getLoc(), otherURCIOp,
                                            urci->getType());
    }
    SingleBlockOwnedForwardingInstFolder folder(*this, urci);
    if (folder.add(otherURCI)) {
      auto *newValue = Builder.createUncheckedRefCast(
          urci->getLoc(), otherURCIOp, urci->getType());
      return std::move(folder).optimizeWithReplacement(newValue);
    }
  }

  // %0 = upcast %x : $X->Y
  // %1 = unchecked_ref_cast %0 : $Y->Z
  //
  // ->
  //
  // %0 = upcast %x : $X->Y
  // %1 = unchecked_ref_cast %x : $X->Z
  //
  // NOTE: For owned values, we only perform this optimization if we can
  // guarantee that we can eliminate the upcast.
  if (auto *ui = dyn_cast<UpcastInst>(urci->getOperand())) {
    SILValue uiOp = ui->getOperand();

    if (uiOp.getOwnershipKind() != OwnershipKind::Owned) {
      return Builder.createUncheckedRefCast(urci->getLoc(), uiOp,
                                            urci->getType());
    }

    SingleBlockOwnedForwardingInstFolder folder(*this, urci);
    if (folder.add(ui)) {
      auto *newValue =
          Builder.createUncheckedRefCast(urci->getLoc(), uiOp, urci->getType());
      return std::move(folder).optimizeWithReplacement(newValue);
    }
  }

  // This is an exact transform where we are replacing urci with an upcast on
  // the same value. So from an ownership perspective because both instructions
  // are forwarding and we are eliminating urci, we are safe.
  if (urci->getType() != urci->getOperand()->getType() &&
      urci->getType().isExactSuperclassOf(urci->getOperand()->getType()))
    return Builder.createUpcast(urci->getLoc(), urci->getOperand(),
                                urci->getType());

  // %0 = init_existential_ref %x : $X -> Existential
  // %1 = open_existential_ref %0 : $Existential -> @opened() Existential
  // %2 = unchecked_ref_cast %1
  //
  // ->
  //
  // %0 = init_existential_ref %x : $X -> Existential
  // %1 = open_existential_ref %0 : $Existential -> @opened() Existential
  // %2 = unchecked_ref_cast %x
  //
  // NOTE: When we have an owned value, we only perform this optimization if we
  // can remove both the open_existential_ref and the init_existential_ref.
  if (auto *oer = dyn_cast<OpenExistentialRefInst>(urci->getOperand())) {
    if (auto *ier = dyn_cast<InitExistentialRefInst>(oer->getOperand())) {
      if (ier->getOwnershipKind() != OwnershipKind::Owned) {
        return Builder.createUncheckedRefCast(urci->getLoc(), ier->getOperand(),
                                              urci->getType());
      }

      SingleBlockOwnedForwardingInstFolder folder(*this, urci);
      if (folder.add(oer) && folder.add(ier)) {
        auto *newValue = Builder.createUncheckedRefCast(
            urci->getLoc(), ier->getOperand(), urci->getType());
        return std::move(folder).optimizeWithReplacement(newValue);
      }
    }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitEndCOWMutationInst(EndCOWMutationInst *ECM) {

  // Remove a cast if it's only used by an end_cow_mutation.
  //
  // (end_cow_mutation (upcast X)) -> (end_cow_mutation X)
  // (end_cow_mutation (unchecked_ref_cast X)) -> (end_cow_mutation X)
  SILValue op = ECM->getOperand();
  if (!isa<UncheckedRefCastInst>(op) && !isa<UpcastInst>(op))
    return nullptr;
  if (!op->hasOneUse())
    return nullptr;

  SingleValueInstruction *refCast = cast<SingleValueInstruction>(op);
  auto *newECM = Builder.createEndCOWMutation(ECM->getLoc(),
                                              refCast->getOperand(0));
  ECM->replaceAllUsesWith(refCast);
  refCast->setOperand(0, newECM);
  refCast->moveAfter(newECM);
  return eraseInstFromFunction(*ECM);
}

SILInstruction *
SILCombiner::visitBridgeObjectToRefInst(BridgeObjectToRefInst *BORI) {
  if (BORI->getFunction()->hasOwnership())
    return nullptr;
  // Fold noop casts through Builtin.BridgeObject.
  // (bridge_object_to_ref (unchecked-ref-cast x BridgeObject) y)
  //  -> (unchecked-ref-cast x y)
  if (auto URC = dyn_cast<UncheckedRefCastInst>(BORI->getOperand()))
    return Builder.createUncheckedRefCast(BORI->getLoc(), URC->getOperand(),
                                          BORI->getType());
  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *urci) {
  // Promote unchecked_ref_cast_addr in between two loadable values to
  // unchecked_ref_cast upon objects.
  //
  // NOTE: unchecked_ref_cast_addr is a taking operation, so we simulate that
  // with objects.
  SILType srcTy = urci->getSrc()->getType();
  if (!srcTy.isLoadable(*urci->getFunction()))
    return nullptr;

  SILType destTy = urci->getDest()->getType();
  if (!destTy.isLoadable(*urci->getFunction()))
    return nullptr;

  // After promoting unchecked_ref_cast_addr to unchecked_ref_cast, the SIL
  // verifier will assert that the loadable source and dest type of reference
  // castable. If the static types are invalid, simply avoid promotion, that way
  // the runtime will then report a failure if this cast is ever executed.
  if (!SILType::canRefCast(srcTy.getObjectType(), destTy.getObjectType(),
                           urci->getModule()))
    return nullptr;
 
  SILLocation loc = urci->getLoc();
  Builder.setCurrentDebugScope(urci->getDebugScope());
  SILValue load = Builder.emitLoadValueOperation(loc, urci->getSrc(),
                                                 LoadOwnershipQualifier::Take);

  assert(SILType::canRefCast(load->getType(), destTy.getObjectType(),
                             Builder.getModule()) &&
         "SILBuilder cannot handle reference-castable types");
  auto *cast = Builder.createUncheckedRefCast(loc, load,
                                              destTy.getObjectType());
  Builder.emitStoreValueOperation(loc, cast, urci->getDest(),
                                  StoreOwnershipQualifier::Init);

  return eraseInstFromFunction(*urci);
}

template <class CastInst>
static bool canBeUsedAsCastDestination(SILValue value, CastInst *castInst,
                                       DominanceAnalysis *DA) {
  return value &&
         value->getType() == castInst->getTargetLoweredType().getObjectType() &&
         DA->get(castInst->getFunction())->properlyDominates(value, castInst);
}


SILInstruction *
SILCombiner::
visitUnconditionalCheckedCastAddrInst(UnconditionalCheckedCastAddrInst *UCCAI) {
  if (UCCAI->getFunction()->hasOwnership())
    return nullptr;

  // Optimize the unconditional_checked_cast_addr in this pattern:
  //
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to %err : $*Error
  //   %dest = alloc_stack $ConcreteError
  //   unconditional_checked_cast_addr Error in %err : $*Error to
  //                                ConcreteError in %dest : $*ConcreteError
  //
  // to:
  //   ...
  //   retain_value %value : $ConcreteError
  //   destroy_addr %err : $*Error
  //   store %value to %dest $*ConcreteError
  //
  // This lets the alloc_existential_box become dead and it can be removed in
  // following optimizations.
  SILValue val = getConcreteValueOfExistentialBoxAddr(UCCAI->getSrc(), UCCAI);
  if (canBeUsedAsCastDestination(val, UCCAI, DA)) {
    SILBuilderContext builderCtx(Builder.getModule(), Builder.getTrackingList());
    SILBuilderWithScope builder(UCCAI, builderCtx);
    SILLocation loc = UCCAI->getLoc();
    builder.createRetainValue(loc, val, builder.getDefaultAtomicity());
    builder.createDestroyAddr(loc, UCCAI->getSrc());
    builder.createStore(loc, val, UCCAI->getDest(),
                        StoreOwnershipQualifier::Unqualified);
    return eraseInstFromFunction(*UCCAI);
  }

  // Perform the purly type-based cast optimization.
  if (CastOpt.optimizeUnconditionalCheckedCastAddrInst(UCCAI))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  if (UCCI->getFunction()->hasOwnership())
    return nullptr;

  if (CastOpt.optimizeUnconditionalCheckedCastInst(UCCI)) {
    MadeChange = true;
    return nullptr;
  }
  // FIXME: rename from RemoveCondFails to RemoveRuntimeAsserts.
  if (RemoveCondFails) {
    auto LoweredTargetType = UCCI->getType();
    auto Loc = UCCI->getLoc();
    auto Op = UCCI->getOperand();
    if (LoweredTargetType.isAddress()) {
      // unconditional_checked_cast -> unchecked_addr_cast
      return Builder.createUncheckedAddrCast(Loc, Op, LoweredTargetType);
    } else if (LoweredTargetType.isHeapObjectReferenceType()) {
      if (!(Op->getType().isHeapObjectReferenceType() ||
            Op->getType().isClassExistentialType())) {
        return nullptr;
      }
      // unconditional_checked_cast -> unchecked_ref_cast
      return Builder.createUncheckedRefCast(Loc, Op, LoweredTargetType);
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitRawPointerToRefInst(RawPointerToRefInst *RawToRef) {
  if (RawToRef->getFunction()->hasOwnership())
    return nullptr;

  // (raw_pointer_to_ref (ref_to_raw_pointer x X->Y) Y->Z)
  //   ->
  // (unchecked_ref_cast X->Z)
  if (auto *RefToRaw = dyn_cast<RefToRawPointerInst>(RawToRef->getOperand())) {
    return Builder.createUncheckedRefCast(RawToRef->getLoc(),
                                          RefToRaw->getOperand(),
                                          RawToRef->getType());
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI) {
  if (UTBCI->getFunction()->hasOwnership())
    return nullptr;

  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_trivial_bit_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  SILValue Op = UTBCI->getOperand();
  if (auto *OtherUTBCI = dyn_cast<UncheckedTrivialBitCastInst>(Op)) {
    return Builder.createUncheckedTrivialBitCast(UTBCI->getLoc(),
                                                 OtherUTBCI->getOperand(),
                                                 UTBCI->getType());
  }

  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_ref_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  if (auto *URBCI = dyn_cast<UncheckedRefCastInst>(Op)) {
    return Builder.createUncheckedTrivialBitCast(UTBCI->getLoc(),
                                                 URBCI->getOperand(),
                                                 UTBCI->getType());
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *UBCI) {
  if (UBCI->getFunction()->hasOwnership())
    return nullptr;

  // (unchecked_bitwise_cast Y->Z (unchecked_bitwise_cast X->Y x))
  // OR (unchecked_trivial_cast Y->Z (unchecked_bitwise_cast X->Y x))
  //   ->
  // (unchecked_bitwise_cast X->Z x)
  SILValue Oper;
  if (match(UBCI->getOperand(),
            m_CombineOr(m_UncheckedBitwiseCastInst(m_SILValue(Oper)),
                        m_UncheckedTrivialBitCastInst(m_SILValue(Oper))))) {
    return Builder.createUncheckedBitwiseCast(UBCI->getLoc(), Oper,
                                              UBCI->getType());
  }
  if (UBCI->getType().isTrivial(*UBCI->getFunction()))
    return Builder.createUncheckedTrivialBitCast(UBCI->getLoc(),
                                                 UBCI->getOperand(),
                                                 UBCI->getType());

  if (!SILType::canRefCast(UBCI->getOperand()->getType(), UBCI->getType(),
                           Builder.getModule()))
    return nullptr;

  return Builder.createUncheckedRefCast(UBCI->getLoc(), UBCI->getOperand(),
                                        UBCI->getType());
}

SILInstruction *
SILCombiner::visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCMI) {
  if (TTOCMI->getFunction()->hasOwnership())
    return nullptr;

  if (auto *OCTTMI = dyn_cast<ObjCToThickMetatypeInst>(TTOCMI->getOperand())) {
    TTOCMI->replaceAllUsesWith(OCTTMI->getOperand());
    return eraseInstFromFunction(*TTOCMI);
  }

  // Perform the following transformations:
  // (thick_to_objc_metatype (metatype @thick)) ->
  // (metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (value_metatype @thick)) ->
  // (value_metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (existential_metatype @thick)) ->
  // (existential_metatype @objc_metatype)
  if (CastOpt.optimizeMetatypeConversion(TTOCMI, MetatypeRepresentation::Thick))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTMI) {
  if (OCTTMI->getFunction()->hasOwnership())
    return nullptr;

  if (auto *TTOCMI = dyn_cast<ThickToObjCMetatypeInst>(OCTTMI->getOperand())) {
    OCTTMI->replaceAllUsesWith(TTOCMI->getOperand());
    return eraseInstFromFunction(*OCTTMI);
  }

  // Perform the following transformations:
  // (objc_to_thick_metatype (metatype @objc_metatype)) ->
  // (metatype @thick)
  //
  // (objc_to_thick_metatype (value_metatype @objc_metatype)) ->
  // (value_metatype @thick)
  //
  // (objc_to_thick_metatype (existential_metatype @objc_metatype)) ->
  // (existential_metatype @thick)
  if (CastOpt.optimizeMetatypeConversion(OCTTMI, MetatypeRepresentation::ObjC))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::visitCheckedCastBranchInst(CheckedCastBranchInst *CBI) {
  if (CBI->getFunction()->hasOwnership())
    return nullptr;

  if (CastOpt.optimizeCheckedCastBranchInst(CBI))
    MadeChange = true;

  return nullptr;
}

SILInstruction *
SILCombiner::
visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI) {
  if (CCABI->getFunction()->hasOwnership())
    return nullptr;

  // Optimize the checked_cast_addr_br in this pattern:
  //
  //   %box = alloc_existential_box $Error, $ConcreteError
  //   %a = project_existential_box $ConcreteError in %b : $Error
  //   store %value to %a : $*ConcreteError
  //   %err = alloc_stack $Error
  //   store %box to %err : $*Error
  //   %dest = alloc_stack $ConcreteError
  //   checked_cast_addr_br <consumption-kind> Error in %err : $*Error to
  //        ConcreteError in %dest : $*ConcreteError, success_bb, failing_bb
  //
  // to:
  //   ...
  //   retain_value %value : $ConcreteError
  //   destroy_addr %err : $*Error           // if consumption-kind is take
  //   store %value to %dest $*ConcreteError
  //   br success_bb
  //
  // This lets the alloc_existential_box become dead and it can be removed in
  // following optimizations.
  //
  // TODO: Also handle the WillFail case.
  SILValue val = getConcreteValueOfExistentialBoxAddr(CCABI->getSrc(), CCABI);
  if (canBeUsedAsCastDestination(val, CCABI, DA)) {
    SILBuilderContext builderCtx(Builder.getModule(), Builder.getTrackingList());
    SILBuilderWithScope builder(CCABI, builderCtx);
    SILLocation loc = CCABI->getLoc();
    builder.createRetainValue(loc, val, builder.getDefaultAtomicity());
    switch (CCABI->getConsumptionKind()) {
      case CastConsumptionKind::TakeAlways:
      case CastConsumptionKind::TakeOnSuccess:
        builder.createDestroyAddr(loc, CCABI->getSrc());
        break;
      case CastConsumptionKind::CopyOnSuccess:
        break;
      case CastConsumptionKind::BorrowAlways:
        llvm_unreachable("BorrowAlways is not supported on addresses");
    }
    builder.createStore(loc, val, CCABI->getDest(),
                        StoreOwnershipQualifier::Unqualified);
                        
    // Replace the cast with a constant conditional branch.
    // Don't just create an unconditional branch to not change the CFG in
    // SILCombine. SimplifyCFG will clean that up.
    //
    // Another possibility would be to run this optimization in SimplifyCFG.
    // But this has other problems, like it's more difficult to reason about a
    // consistent dominator tree in SimplifyCFG.
    SILType boolTy = SILType::getBuiltinIntegerType(1, builder.getASTContext());
    auto *trueVal = builder.createIntegerLiteral(loc, boolTy, 1);
    builder.createCondBranch(loc, trueVal, CCABI->getSuccessBB(),
                             CCABI->getFailureBB());
    return eraseInstFromFunction(*CCABI);
  }

  // Perform the purly type-based cast optimization.
  if (CastOpt.optimizeCheckedCastAddrBranchInst(CCABI))
    MadeChange = true;

  return nullptr;
}

SILInstruction *SILCombiner::visitConvertEscapeToNoEscapeInst(
    ConvertEscapeToNoEscapeInst *Cvt) {
  auto *OrigThinToThick =
      dyn_cast<ThinToThickFunctionInst>(Cvt->getConverted());
  if (!OrigThinToThick)
    return nullptr;
  auto origFunType = OrigThinToThick->getType().getAs<SILFunctionType>();
  auto NewTy = origFunType->getWithExtInfo(origFunType->getExtInfo().withNoEscape(true));

  return Builder.createThinToThickFunction(
      OrigThinToThick->getLoc(), OrigThinToThick->getOperand(),
      SILType::getPrimitiveObjectType(NewTy));
}

SILInstruction *SILCombiner::visitConvertFunctionInst(ConvertFunctionInst *CFI) {
  if (CFI->getFunction()->hasOwnership())
    return nullptr;

  // If this conversion only changes substitutions, then rewrite applications
  // of the converted function as applications of the original.
  //
  // (full_apply (convert_function[only_converts_substitutions] x)) => (full_apply x)
  // (partial_apply (convert_function[only_converts_substitutions] x)) => (convert_function (partial_apply x))
  //
  // TODO: We could generalize this to handle other ABI-compatible cases, by
  // inserting the necessary casts around the arguments.
  if (CFI->onlyConvertsSubstitutions()) {
    auto usei = CFI->use_begin();
    while (usei != CFI->use_end()) {
      auto use = *usei++;
      auto user = use->getUser();
      if (isa<ApplySite>(user) && use->getOperandNumber() == 0) {
        auto applySite = ApplySite(user);
        // If this is a partial_apply, insert a convert_function back to the
        // original result type.

        if (auto pa = dyn_cast<PartialApplyInst>(user)) {
          auto partialApplyTy = pa->getType();
          Builder.setInsertionPoint(std::next(pa->getIterator()));
          
          SmallVector<SILValue, 4> args(pa->getArguments().begin(),
                                        pa->getArguments().end());
          
          auto newPA = Builder.createPartialApply(pa->getLoc(),
                                  CFI->getConverted(),
                                  pa->getSubstitutionMap(),
                                  args,
                                  pa->getFunctionType()->getCalleeConvention());
          auto newConvert = Builder.createConvertFunction(pa->getLoc(),
                                                          newPA, partialApplyTy,
                                                          false);
          pa->replaceAllUsesWith(newConvert);
          eraseInstFromFunction(*pa);
          
          continue;
        }
        
        // For full apply sites, we only need to replace the `convert_function`
        // with the original value.
        use->set(CFI->getConverted());
        applySite.setSubstCalleeType(
                      CFI->getConverted()->getType().castTo<SILFunctionType>());
      }
    }
  }
  
  // (convert_function (convert_function x)) => (convert_function x)
  if (auto subCFI = dyn_cast<ConvertFunctionInst>(CFI->getConverted())) {
    // If we convert the function type back to itself, we can replace the
    // conversion completely.
    if (subCFI->getConverted()->getType() == CFI->getType()) {
      CFI->replaceAllUsesWith(subCFI->getConverted());
      eraseInstFromFunction(*CFI);
      return nullptr;
    }
    
    // Otherwise, we can still bypass the intermediate conversion.
    CFI->getOperandRef().set(subCFI->getConverted());
  }
  
  // Replace a convert_function that only has refcounting uses with its
  // operand.
  auto anyNonRefCountUse =
    std::any_of(CFI->use_begin(),
                CFI->use_end(),
                [](Operand *Use) {
                  return !isa<RefCountingInst>(Use->getUser());
                });

  if (anyNonRefCountUse)
    return nullptr;

  // Replace all retain/releases on convert_function by retain/releases on
  // its argument. This is required to preserve the lifetime of its argument,
  // which could be e.g. a partial_apply instruction capturing some further
  // arguments.
  auto Converted = CFI->getConverted();
  while (!CFI->use_empty()) {
    auto *Use = *(CFI->use_begin());
    assert(Use->getUser()->getResults().empty() &&
           "Did not expect user with a result!");
    Use->set(Converted);
  }

  eraseInstFromFunction(*CFI);
  return nullptr;
}
