//===--- TypeCheckProtocol.cpp - Protocol Checking ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for protocols, in particular, checking
// whether a given type conforms to a given protocol.
//===----------------------------------------------------------------------===//

#include "ConstraintSystem.h"
#include "TypeChecker.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/NameLookup.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/SmallString.h"

using namespace swift;

/// \brief Retrieve the kind of requirement described by the given declaration,
/// for use in some diagnostics.
/// FIXME: Enumify this.
int getRequirementKind(ValueDecl *VD) {
  if (isa<FuncDecl>(VD))
    return 0;

  if (isa<VarDecl>(VD))
    return 1;
  
  assert(isa<SubscriptDecl>(VD) && "Unhandled requirement kind");
  return 2;
}

namespace {
  /// \brief The result of matching a particular declaration to a given
  /// requirement.
  enum class MatchKind : unsigned char {
    /// \brief The witness matched the requirement exactly.
    ExactMatch,

    /// \brief The witness matched the requirement with some renaming.
    RenamedMatch,

    /// \brief The witness is invalid or has an invalid type.
    WitnessInvalid,

    /// \brief The kind of the witness and requirement differ, e.g., one
    /// is a function and the other is a variable.
    KindConflict,

    /// \brief The types conflict.
    TypeConflict,

    /// \brief The witness did not match due to static/non-static differences.
    StaticNonStaticConflict,

    /// \brief The witness did not match due to prefix/non-prefix differences.
    PrefixNonPrefixConflict,

    /// \brief The witness did not match due to postfix/non-postfix differences.
    PostfixNonPostfixConflict
  };

  /// \brief Describes a match between a requirement and a witness.
  struct RequirementMatch {
    RequirementMatch(ValueDecl *witness, MatchKind kind,
                     Type witnessType = Type())
      : Witness(witness), Kind(kind), WitnessType(witnessType)
    {
      assert(hasWitnessType() == !witnessType.isNull() &&
             "Should (or should not) have witness type");
    }
    
    /// \brief The witness that matches the (implied) requirement.
    ValueDecl *Witness;
    
    /// \brief The kind of match.
    MatchKind Kind;

    /// \brief The type of the witness when it is referenced.
    Type WitnessType;

    /// \brief Determine whether this match is viable.
    bool isViable() const {
      switch(Kind) {
      case MatchKind::ExactMatch:
      case MatchKind::RenamedMatch:
        return true;

      case MatchKind::WitnessInvalid:
      case MatchKind::KindConflict:
      case MatchKind::TypeConflict:
      case MatchKind::StaticNonStaticConflict:
      case MatchKind::PrefixNonPrefixConflict:
      case MatchKind::PostfixNonPostfixConflict:
        return false;
      }
    }

    /// \brief Determine whether this requirement match has a witness type.
    bool hasWitnessType() const {
      switch(Kind) {
      case MatchKind::ExactMatch:
      case MatchKind::RenamedMatch:
      case MatchKind::TypeConflict:
        return true;

      case MatchKind::WitnessInvalid:
      case MatchKind::KindConflict:
      case MatchKind::StaticNonStaticConflict:
      case MatchKind::PrefixNonPrefixConflict:
      case MatchKind::PostfixNonPostfixConflict:
        return false;
      }
    }

    /// FIXME: Generic substitutions here.

    /// \brief Associated types determined by matching this requirement.
    SmallVector<std::pair<TypeAliasDecl *, Type>, 2> AssociatedTypeDeductions;
    
    /// \brief Associated type substitutions needed to match the witness.
    SmallVector<Substitution, 2> WitnessSubstitutions;
  };
}

///\ brief Decompose the given type into a set of tuple elements.
static SmallVector<TupleTypeElt, 4> decomposeIntoTupleElements(Type type) {
  SmallVector<TupleTypeElt, 4> result;

  if (auto tupleTy = dyn_cast<TupleType>(type.getPointer())) {
    result.append(tupleTy->getFields().begin(), tupleTy->getFields().end());
    return result;
  }

  result.push_back(type);
  return result;
}

/// \brief Match the given witness to the given requirement.
///
/// \returns the result of performing the match.
static RequirementMatch
matchWitness(TypeChecker &tc, ProtocolDecl *protocol,
             ValueDecl *req, Type reqType,
             Type model, ValueDecl *witness,
             ArrayRef<TypeAliasDecl *> unresolvedAssocTypes) {
  assert(!req->isInvalid() && "Cannot have an invalid requirement here");

  /// Make sure the witness is of the same kind as the requirement.
  if (req->getKind() != witness->getKind())
    return RequirementMatch(witness, MatchKind::KindConflict);

  // If the witness is invalid, record that and stop now.
  if (witness->isInvalid())
    return RequirementMatch(witness, MatchKind::WitnessInvalid);

  // Get the requirement and witness attributes.
  const auto &reqAttrs = req->getAttrs();
  const auto &witnessAttrs = witness->getAttrs();

  // Compute the type of the witness, below.
  Type witnessType;
  bool decomposeFunctionType = false;

  // Check properties specific to functions.
  if (auto funcReq = dyn_cast<FuncDecl>(req)) {
    auto funcWitness = cast<FuncDecl>(witness);

    // Either both must be 'static' or neither.
    if (funcReq->isStatic() != funcWitness->isStatic())
      return RequirementMatch(witness, MatchKind::StaticNonStaticConflict);

    // If we require a prefix operator and the witness is not a prefix operator,
    // these don't match.
    if (reqAttrs.isPrefix() && !witnessAttrs.isPrefix())
      return RequirementMatch(witness, MatchKind::PrefixNonPrefixConflict);

    // If we require a postfix operator and the witness is not a postfix
    // operator, these don't match.
    if (reqAttrs.isPostfix() && !witnessAttrs.isPostfix())
      return RequirementMatch(witness, MatchKind::PostfixNonPostfixConflict);

    // Determine the witness type.
    witnessType = witness->getType();

    // If the witness resides within a type context, substitute through the
    // based type and ignore 'this'.
    if (witness->getDeclContext()->isTypeContext()) {
      witnessType = witness->getType()->castTo<AnyFunctionType>()->getResult();
      witnessType = tc.substMemberTypeWithBase(witnessType, witness, model);
      assert(witnessType && "Cannot refer to witness?");
    }

    // We want to decompose the parameters to handle them separately.
    decomposeFunctionType = true;
  } else {
    // FIXME: Static variables will have to check static vs. non-static here.

    // The witness type is the type of the declaration with the base
    // substituted.
    witnessType = tc.substMemberTypeWithBase(witness->getType(), witness,
                                             model);
    assert(witnessType && "Cannot refer to witness?");

    // Decompose the parameters for subscript declarations.
    decomposeFunctionType = isa<SubscriptDecl>(req);
  }

  // Construct a constraint system to use to solve the equality between
  // the required type and the witness type.
  // FIXME: Pass the nominal/extension context in as the DeclContext?
  constraints::ConstraintSystem cs(tc, &tc.TU);

  // Open up the type of the requirement and witness, replacing any unresolved
  // archetypes with type variables.
  llvm::DenseMap<ArchetypeType *, TypeVariableType *> replacements;
  SmallVector<ArchetypeType *, 4> unresolvedArchetypes;
  if (!unresolvedAssocTypes.empty()) {
    for (auto assoc : unresolvedAssocTypes)
      unresolvedArchetypes.push_back(
        assoc->getDeclaredType()->castTo<ArchetypeType>());

    reqType = cs.openType(reqType, unresolvedArchetypes, replacements);
  }
  
  llvm::DenseMap<ArchetypeType *, TypeVariableType *> witnessReplacements;
  SmallVector<ArchetypeType *, 4> witnessArchetypes;
  auto openWitnessType = cs.openType(witnessType, witnessArchetypes,
                                     witnessReplacements);

  bool anyRenaming = false;
  if (decomposeFunctionType) {
    // Decompose function types into parameters and result type.
    auto reqInputType = reqType->castTo<AnyFunctionType>()->getInput();
    auto reqResultType = reqType->castTo<AnyFunctionType>()->getResult();
    auto witnessInputType = openWitnessType->castTo<AnyFunctionType>()->getInput();
    auto witnessResultType = openWitnessType->castTo<AnyFunctionType>()->getResult();

    // Result types must match.
    // FIXME: Could allow (trivial?) subtyping here.
    cs.addConstraint(constraints::ConstraintKind::Equal,
                     witnessResultType, reqResultType);
    // FIXME: Check whether this has already failed.

    // Parameter types and kinds must match. Start by decomposing the input
    // types into sets of tuple elements.
    // Decompose the input types into parameters.
    auto reqParams = decomposeIntoTupleElements(reqInputType);
    auto witnessParams = decomposeIntoTupleElements(witnessInputType);

    // If the number of parameters doesn't match, we're done.
    if (reqParams.size() != witnessParams.size())
      return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

    // Match each of the parameters.
    for (unsigned i = 0, n = reqParams.size(); i != n; ++i) {
      // Variadic bits must match.
      // FIXME: Specialize the match failure kind
      if (reqParams[i].isVararg() != witnessParams[i].isVararg())
        return RequirementMatch(witness, MatchKind::TypeConflict, witnessType);

      // Check the parameter names.
      if (reqParams[i].getName() != witnessParams[i].getName()) {
        // A parameter has been renamed.
        anyRenaming = true;

        // For an Objective-C requirement, all but the first parameter name is
        // significant.
        // FIXME: Specialize the match failure kind.
        // FIXME: Constructors care about the first name.
        if (protocol->getAttrs().isObjC() && i > 0)
          return RequirementMatch(witness, MatchKind::TypeConflict,
                                  witnessType);
      }

      // Check whether the parameter types match.
      cs.addConstraint(constraints::ConstraintKind::Equal,
                       witnessParams[i].getType(), reqParams[i].getType());
      // FIXME: Check whether this failed.

      // FIXME: Consider default arguments here?
    }
  } else {
    // Simple case: add the constraint.
    cs.addConstraint(constraints::ConstraintKind::Equal,
                     openWitnessType, reqType);
  }

  // Try to solve the system.
  SmallVector<constraints::Solution, 1> solutions;
  if (cs.solve(solutions, /*allowFreeTypeVariables=*/true)) {
    return RequirementMatch(witness, MatchKind::TypeConflict,
                            witnessType);
  }
  auto &solution = solutions.front();
  
  // Success. Form the match result.
  RequirementMatch result(witness,
                          anyRenaming? MatchKind::RenamedMatch
                                     : MatchKind::ExactMatch,
                          witnessType);

  // If we deduced any associated types, record them in the result.
  if (!replacements.empty()) {
    for (auto assocType : unresolvedAssocTypes) {
      auto archetype = assocType->getDeclaredType()->castTo<ArchetypeType>();
      auto known = replacements.find(archetype);
      if (known == replacements.end())
        continue;

      auto replacement = solution.simplifyType(tc, known->second);
      assert(replacement && "Couldn't simplify type variable?");

      // If the replacement still contains a type variable, we didn't deduce it.
      if (replacement->hasTypeVariable())
        continue;

      result.AssociatedTypeDeductions.push_back({assocType, replacement});
    }
  }
  
  // Save archetype mappings we deduced for the witness.
  for (auto &witnessReplacement : witnessReplacements) {
    auto archetype = witnessReplacement.first;
    auto typeVar = witnessReplacement.second;
    
    auto sub = solution.simplifyType(tc, typeVar);
    assert(sub && "couldn't simplify type variable?");
    
    assert(!sub->hasTypeVariable() && "type variable in witness sub");
    
    // Produce conformances for the substitution.
    SmallVector<ProtocolConformance*, 2> conformances;
    for (auto archetypeProto : archetype->getConformsTo()) {
      ProtocolConformance *conformance = nullptr;
      bool conformed = tc.conformsToProtocol(sub, archetypeProto, &conformance);
      assert(conformed &&
             "archetype substitution did not conform to requirement?");
      (void)conformed;
      conformances.push_back(conformance);
    }
    
    result.WitnessSubstitutions.push_back({archetype, sub,
                                        tc.Context.AllocateCopy(conformances)});
  }

  return result;
}

/// \brief Determine whether one requirement match is better than the other.
static bool isBetterMatch(const RequirementMatch &match1,
                          const RequirementMatch &match2) {
  // Earlier match kinds are better. This prefers exact matches over matches
  // that require renaming, for example.
  if (match1.Kind != match2.Kind)
    return static_cast<unsigned>(match1.Kind)
             < static_cast<unsigned>(match2.Kind);

  // FIXME: Should use the same "at least as specialized as" rules as overload
  // resolution.
  return false;
}

/// \brief Add the next associated type deduction to the string representation
/// of the deductions, used in diagnostics.
static void addAssocTypeDeductionString(llvm::SmallString<128> &str,
                                        TypeAliasDecl *assocType,
                                        Type deduced) {
  if (str.empty())
    str = " [with ";
  else
    str += ", ";

  str += assocType->getName().str();
  str += " = ";
  str += deduced.getString();
}

/// \brief Diagnose a requirement match, describing what went wrong (or not).
static void
diagnoseMatch(TypeChecker &tc, ValueDecl *req,
              const RequirementMatch &match,
              ArrayRef<std::pair<TypeAliasDecl *, Type>> deducedAssocTypes) {
  // Form a string describing the associated type deductions.
  // FIXME: Determine which associated types matter, and only print those.
  llvm::SmallString<128> withAssocTypes;
  for (const auto &deduced : deducedAssocTypes) {
    addAssocTypeDeductionString(withAssocTypes, deduced.first, deduced.second);
  }
  for (const auto &deduced : match.AssociatedTypeDeductions) {
    addAssocTypeDeductionString(withAssocTypes, deduced.first, deduced.second);
  }
  if (!withAssocTypes.empty())
    withAssocTypes += "]";

  switch (match.Kind) {
  case MatchKind::ExactMatch:
    tc.diagnose(match.Witness, diag::protocol_witness_exact_match,
                withAssocTypes);
    break;

  case MatchKind::RenamedMatch:
    tc.diagnose(match.Witness, diag::protocol_witness_renamed, withAssocTypes);
    break;

  case MatchKind::KindConflict:
    tc.diagnose(match.Witness, diag::protocol_witness_kind_conflict,
                getRequirementKind(req));
    break;

  case MatchKind::WitnessInvalid:
    // Don't bother to diagnose invalid witnesses; we've already complained
    // about them.
    break;

  case MatchKind::TypeConflict:
    tc.diagnose(match.Witness, diag::protocol_witness_type_conflict,
                match.WitnessType, withAssocTypes);
    break;

  case MatchKind::StaticNonStaticConflict:
    // FIXME: Could emit a Fix-It here.
    tc.diagnose(match.Witness, diag::protocol_witness_static_conflict,
                !req->isInstanceMember());
    break;

  case MatchKind::PrefixNonPrefixConflict:
    // FIXME: Could emit a Fix-It here.
    tc.diagnose(match.Witness, diag::protocol_witness_prefix_postfix_conflict,
                false, match.Witness->getAttrs().isPostfix()? 2 : 0);
    break;

  case MatchKind::PostfixNonPostfixConflict:
    // FIXME: Could emit a Fix-It here.
    tc.diagnose(match.Witness, diag::protocol_witness_prefix_postfix_conflict,
                true, match.Witness->getAttrs().isPrefix() ? 1 : 0);
    break;
  }
}

/// Compute the substitution for the given archetype and its replacement
/// type.
static Substitution getArchetypeSubstitution(TypeChecker &tc,
                                             ArchetypeType *archetype,
                                             Type replacement) {
  Substitution result;
  result.Archetype = archetype;
  result.Replacement = replacement;
  llvm::SmallVector<ProtocolConformance *, 4> conformances;

  for (auto proto : archetype->getConformsTo()) {
    ProtocolConformance *conformance = nullptr;
    bool conforms = tc.conformsToProtocol(replacement, proto, &conformance);
    assert(conforms && "Conformance should already have been verified");
    conformances.push_back(conformance);
  }

  result.Conformance = tc.Context.AllocateCopy(conformances);
  return result;
}

/// \brief Determine whether the type \c T conforms to the protocol \c Proto,
/// recording the complete witness table if it does.
static ProtocolConformance *
checkConformsToProtocol(TypeChecker &TC, Type T, ProtocolDecl *Proto,
                        Decl *ExplicitConformance,
                        SourceLoc ComplainLoc) {
  WitnessMap Mapping;
  TypeWitnessMap TypeWitnesses;
  TypeSubstitutionMap TypeMapping;
  InheritedConformanceMap InheritedMapping;

  // Check that T conforms to all inherited protocols.
  for (auto InheritedProto : Proto->getProtocols()) {
    ProtocolConformance *InheritedConformance = nullptr;
    if (TC.conformsToProtocol(T, InheritedProto, &InheritedConformance,
                              ComplainLoc))
      InheritedMapping[InheritedProto] = InheritedConformance;
    else {
      // Recursive call already diagnosed this problem, but tack on a note
      // to establish the relationship.
      if (ComplainLoc.isValid()) {
        TC.diagnose(Proto,
                    diag::inherited_protocol_does_not_conform, T,
                    InheritedProto->getDeclaredType());
      }
      return nullptr;
    }
  }
  
  // If the protocol requires a class, non-classes are a non-starter.
  if (Proto->getAttrs().isClassProtocol()
      && !T->getClassOrBoundGenericClass()) {
    if (ComplainLoc.isValid())
      TC.diagnose(ComplainLoc,
                  diag::non_class_cannot_conform_to_class_protocol,
                  T, Proto->getDeclaredType());
    return nullptr;
  }

  bool Complained = false;
  auto metaT = MetaTypeType::get(T, TC.Context);
  
  // First, resolve any associated type members that have bindings. We'll
  // attempt to deduce any associated types that don't have explicit
  // definitions.
  SmallVector<TypeAliasDecl *, 4> unresolvedAssocTypes;
  for (auto Member : Proto->getMembers()) {
    auto AssociatedType = dyn_cast<TypeAliasDecl>(Member);
    if (!AssociatedType)
      continue;
    
    // Bind the implicit 'This' type to the type T.
    // FIXME: Should have some kind of 'implicit' bit to detect this.
    auto archetype
      = AssociatedType->getUnderlyingType()->castTo<ArchetypeType>();
    if (AssociatedType->getName().str().equals("This")) {
      TypeMapping[archetype]= T;
      continue;
    }

    auto candidates = TC.lookupMemberType(metaT, AssociatedType->getName());

    // If we didn't find any matches, consider this associated type to be
    // unresolved.
    if (!candidates) {
      unresolvedAssocTypes.push_back(AssociatedType);
      continue;
    }

    SmallVector<std::pair<TypeDecl *, Type>, 2> Viable;
    SmallVector<std::pair<TypeDecl *, ProtocolDecl *>, 2> NonViable;

    for (auto candidate : candidates) {
      // Check this type against the protocol requirements.
      // FIXME: Check superclass requirement as well.
      bool SatisfiesRequirements = true;
      for (auto ReqProto : AssociatedType->getProtocols()) {
        if (!TC.conformsToProtocol(candidate.second, ReqProto)){
          SatisfiesRequirements = false;

          NonViable.push_back({candidate.first, ReqProto});
          break;
        }

        if (!SatisfiesRequirements)
          break;
      }

      if (SatisfiesRequirements)
        Viable.push_back(candidate);
    }
    
    if (Viable.size() == 1) {
      auto archetype
        = AssociatedType->getUnderlyingType()->getAs<ArchetypeType>();
      TypeMapping[archetype] = Viable.front().second;
      TypeWitnesses[AssociatedType]
        = getArchetypeSubstitution(TC, archetype, Viable.front().second);
      continue;
    }
    
    if (ComplainLoc.isInvalid())
      return nullptr;
    
    if (!Viable.empty()) {
      if (!Complained) {
        TC.diagnose(ComplainLoc, diag::type_does_not_conform,
                    T, Proto->getDeclaredType());
        Complained = true;
      }
      
      TC.diagnose(AssociatedType,
                  diag::ambiguous_witnesses_type,
                  AssociatedType->getName());
      
      for (auto Candidate : Viable)
        TC.diagnose(Candidate.first, diag::protocol_witness_type);
      
      TypeMapping[archetype] = ErrorType::get(TC.Context);
      continue;
    }

    if (!NonViable.empty()) {
      if (!Complained) {
        TC.diagnose(ComplainLoc, diag::type_does_not_conform,
                    T, Proto->getDeclaredType());
        Complained = true;
      }

      TC.diagnose(AssociatedType, diag::no_witnesses_type,
                  AssociatedType->getName());

      for (auto Candidate : NonViable) {
        TC.diagnose(Candidate.first,
                    diag::protocol_witness_nonconform_type,
                    Candidate.first->getDeclaredType(),
                    Candidate.second->getDeclaredType());
      }

      TypeMapping[archetype] = ErrorType::get(TC.Context);
      continue;
    }
    
    if (ComplainLoc.isValid()) {
      if (!Complained) {
        TC.diagnose(ComplainLoc, diag::type_does_not_conform,
                    T, Proto->getDeclaredType());
        Complained = true;
      }
      
      TC.diagnose(AssociatedType, diag::no_witnesses_type,
                  AssociatedType->getName());
      for (auto candidate : candidates)
        TC.diagnose(candidate.first, diag::protocol_witness_type);
      
      TypeMapping[archetype] = ErrorType::get(TC.Context);
    } else {
      return nullptr;
    }
  }

  // If we complain about any associated types, there is no point in continuing.
  if (Complained)
    return nullptr;

  // Check that T provides all of the required func/variable/subscript members.
  SmallVector<std::pair<TypeAliasDecl *, Type>, 4> deducedAssocTypes;
  for (auto Member : Proto->getMembers()) {
    auto Requirement = dyn_cast<ValueDecl>(Member);
    if (!Requirement)
      continue;

    // Associated type requirements handled above.
    if (isa<TypeAliasDecl>(Requirement))
      continue;

    // Determine the type that the requirement is expected to have. If the
    // requirement is for a function, look past the 'this' parameter.
    Type reqType = Requirement->getType();
    if (isa<FuncDecl>(Requirement))
      reqType = reqType->castTo<AnyFunctionType>()->getResult();

    // Substitute the type mappings we have into the requirement type.
    reqType = TC.substType(reqType, TypeMapping, /*IgnoreMissing=*/true);
    assert(reqType && "We didn't check our type mappings?");

    // Gather the witnesses.
    SmallVector<ValueDecl *, 4> witnesses;
    if (Requirement->getName().isOperator()) {
      // Operator lookup is always global.
      UnqualifiedLookup Lookup(Requirement->getName(), &TC.TU);

      if (Lookup.isSuccess()) {
        for (auto Candidate : Lookup.Results) {
          assert(Candidate.hasValueDecl());
          witnesses.push_back(Candidate.getValueDecl());
        }
      }
    } else {
      // Variable/function/subscript requirements.
      for (auto candidate : TC.lookupMember(metaT, Requirement->getName())) {
        witnesses.push_back(candidate);
      }
    }

    // Match each of the witnesses to the requirement, to see which ones
    // succeed.
    SmallVector<RequirementMatch, 4> matches;
    unsigned numViable = 0;
    unsigned bestIdx = 0;
    for (auto witness : witnesses) {
      // Don't match anything in a protocol.
      // FIXME: When default implementations come along, we can try to match
      // these when they're default implementations coming from another
      // (unrelated) protocol.
      if (isa<ProtocolDecl>(witness->getDeclContext())) {
        continue;
      }

      auto match = matchWitness(TC, Proto, Requirement, reqType, T, witness,
                                unresolvedAssocTypes);
      if (match.isViable()) {
        ++numViable;
        bestIdx = matches.size();
      }

      matches.push_back(std::move(match));
    }

    // If there are any viable matches, try to find the best.
    if (numViable >= 1) {
      // If there numerous viable matches, throw out the non-viable matches
      // and try to find a "best" match.
      bool isReallyBest = true;
      if (numViable > 1) {
        matches.erase(std::remove_if(matches.begin(), matches.end(),
                                     [](const RequirementMatch &match) {
                                       return !match.isViable();
                                     }),
                        matches.end());

        // Find the best match.
        bestIdx = 0;
        for (unsigned i = 1, n = matches.size(); i != n; ++i) {
          if (isBetterMatch(matches[i], matches[bestIdx]))
            bestIdx = i;
        }

        // Make sure it is, in fact, the best.
        for (unsigned i = 0, n = matches.size(); i != n; ++i) {
          if (i == bestIdx)
            continue;

          if (!isBetterMatch(matches[bestIdx], matches[i])) {
            isReallyBest = false;
            break;
          }
        }
      }

      // If we really do have a best match, record it.
      if (isReallyBest) {
        auto &best = matches[bestIdx];

        // Record the match.
        Mapping[Requirement].Decl = best.Witness;
        Mapping[Requirement].Substitutions
          = TC.Context.AllocateCopy(best.WitnessSubstitutions);

        // If we deduced any associated types, record them now.
        if (!best.AssociatedTypeDeductions.empty()) {
          // Record the deductions.
          for (auto deduction : best.AssociatedTypeDeductions) {
            auto assocType = deduction.first;
            auto archetype = assocType->getDeclaredType()->castTo<ArchetypeType>();
            TypeMapping[archetype] = deduction.second;

            // Compute the archetype substitution.
            TypeWitnesses[assocType]
              = getArchetypeSubstitution(TC, archetype, deduction.second);
          }

          // Remove the now-resolved associated types from the set of
          // unresolved associated types.
          unresolvedAssocTypes.erase(
            std::remove_if(unresolvedAssocTypes.begin(),
                           unresolvedAssocTypes.end(),
                           [&](TypeAliasDecl *assocType) {
                             auto archetype
                               = assocType->getDeclaredType()
                                   ->castTo<ArchetypeType>();

                             auto known = TypeMapping.find(archetype);
                             if (known == TypeMapping.end())
                               return false;

                             deducedAssocTypes.push_back({assocType,
                                                          known->second});
                             return true;
                           }),
            unresolvedAssocTypes.end());
        }

        continue;
      }

      // We have an ambiguity; diagnose it below.
    }

    // We have either no matches or an ambiguous match. Diagnose it.

    // If we're not supposed to complain, don't; just return null to indicate
    // failure.
    if (ComplainLoc.isInvalid())
      return nullptr;

    // Complain that this type does not conform to this protocol.
    if (!Complained) {
      TC.diagnose(ComplainLoc, diag::type_does_not_conform,
                  T, Proto->getDeclaredType());
      Complained = true;
    }

    // Point out the requirement that wasn't met.
    TC.diagnose(Requirement,
                numViable > 0? diag::ambiguous_witnesses
                             : diag::no_witnesses,
                getRequirementKind(Requirement),
                Requirement->getName(),
                reqType);

    // Diagnose each of the matches.
    for (const auto &match : matches)
      diagnoseMatch(TC, Requirement, match, deducedAssocTypes);

    // FIXME: Suggest a new declaration that does match?
  }
  
  if (Complained)
    return nullptr;

  // If any associated types were left unresolved, diagnose them.
  if (!unresolvedAssocTypes.empty()) {
    if (ComplainLoc.isInvalid())
      return nullptr;

    // Diagnose all missing associated types.
    for (auto assocType : unresolvedAssocTypes) {
      if (!Complained) {
        TC.diagnose(ComplainLoc, diag::type_does_not_conform,
                    T, Proto->getDeclaredType());
        Complained = true;
      }

      TC.diagnose(assocType, diag::no_witnesses_type,
                  assocType->getName());
    }

    return nullptr;
  }

  llvm::SmallVector<ValueDecl *, 4> defaultedDefinitions;
  for (auto deduced : deducedAssocTypes) {
    defaultedDefinitions.push_back(deduced.first);
  }

  Module *conformingModule = ExplicitConformance
    ? ExplicitConformance->getModuleContext()
    : nullptr;

  return TC.Context.getConformance(T, Proto, conformingModule,
                                   std::move(Mapping),
                                   std::move(TypeWitnesses),
                                   std::move(InheritedMapping),
                                   defaultedDefinitions);
}

/// \brief Check whether an existential value of the given protocol conforms
/// to itself.
///
/// \param tc The type checker.
/// \param type The existential type we're checking, used for diagnostics.
/// \param proto The protocol to test.
/// \param If we're allowed to complain, the location to use.

/// \returns true if the existential type conforms to itself, false otherwise.
static bool
existentialConformsToItself(TypeChecker &tc,
                            Type type,
                            ProtocolDecl *proto,
                            SourceLoc complainLoc,
                            llvm::SmallPtrSet<ProtocolDecl *, 4> &checking) {
  // If we already know whether this protocol's existential conforms to itself
  // use the cached value... unless it's negative and we're supposed to
  // complain, in which case we fall through.
  if (auto known = proto->existentialConformsToSelf()) {
    if (*known || complainLoc.isInvalid())
      return *known;
  }

  // Check that all inherited protocols conform to themselves.
  for (auto inheritedProto : proto->getProtocols()) {
    // If we're already checking this protocol, assume it's fine.
    if (!checking.insert(inheritedProto))
      continue;

    // Check whether the inherited protocol conforms to itself.
    if (!existentialConformsToItself(tc, type, inheritedProto, complainLoc,
                                     checking)) {
      // Recursive call already diagnosed this problem, but tack on a note
      // to establish the relationship.
      // FIXME: Poor location information.
      if (complainLoc.isValid()) {
        tc.diagnose(proto,
                    diag::inherited_protocol_does_not_conform, type,
                    inheritedProto->getType());
      }

      proto->setExistentialConformsToSelf(false);
      return false;
    }
  }

  // Check whether this protocol conforms to itself.
  auto thisDecl = proto->getThis();
  auto thisType =proto->getThis()->getUnderlyingType()->castTo<ArchetypeType>();
  for (auto member : proto->getMembers()) {
    // Check for associated types.
    if (auto associatedType = dyn_cast<TypeAliasDecl>(member)) {
      // 'This' is obviously okay.
      if (associatedType == thisDecl)
        continue;

      // A protocol cannot conform to itself if it has an associated type.
      proto->setExistentialConformsToSelf(false);
      if (complainLoc.isInvalid())
        return false;

      tc.diagnose(complainLoc, diag::type_does_not_conform, type,
                  proto->getDeclaredType());
      tc.diagnose(associatedType, diag::protocol_existential_assoc_type,
                  associatedType->getName());
      return false;
    }

    // For value members, look at their type signatures.
    auto valueMember = dyn_cast<ValueDecl>(member);
    if (!valueMember)
      continue;

    // Extract the type of the member, ignoring the 'this' parameter of
    // functions.
    auto memberTy = valueMember->getType();
    if (memberTy->is<ErrorType>())
      continue;
    if (isa<FuncDecl>(valueMember))
      memberTy = memberTy->castTo<AnyFunctionType>()->getResult();

    // "Transform" the type to walk the whole type. If we find 'This', return
    // null. Otherwise, make this the identity transform and throw away the
    // result.
    if (tc.transformType(memberTy, [&](Type type) -> Type {
          // If we found our archetype, return null.
          if (auto archetype = type->getAs<ArchetypeType>()) {
            return archetype == thisType? nullptr : type;
          }

          return type;
        })) {
      // We didn't find 'This'. We're okay.
      continue;
    }

    // A protocol cannot conform to itself if any of its value members
    // refers to 'This'.
    proto->setExistentialConformsToSelf(false);
    if (complainLoc.isInvalid())
      return false;

    tc.diagnose(complainLoc, diag::type_does_not_conform, type,
                proto->getDeclaredType());
    tc.diagnose(valueMember, diag::protocol_existential_refers_to_this,
                valueMember->getName());
    return false;
  }

  proto->setExistentialConformsToSelf(true);
  return true;
}

/// Retrieve the given declaration context as either a nominal or extension
/// declaration, or null if it is neither.
static Decl *getNominalOrExtensionDecl(DeclContext *dc) {
  if (auto nominal = dyn_cast<NominalTypeDecl>(dc))
    return nominal;

  return dyn_cast<ExtensionDecl>(dc);
}

/// Given an implicitly-generated protocol conformance, complain and
/// suggest explicit conformance.
static void suggestExplicitConformance(TypeChecker &tc,
                                       SourceLoc complainLoc,
                                       Type type,
                                       ProtocolConformance *conformance) {
  auto proto = conformance->getProtocol();

  // Complain that we don't have explicit conformance.
  tc.diagnose(complainLoc, diag::type_does_not_explicitly_conform,
              type, proto->getDeclaredType());

  // Figure out where to hang the explicit conformance for the Fix-It.
  Decl *owner = nullptr;
  for (auto req : proto->getMembers()) {
    auto valueReq = dyn_cast<ValueDecl>(req);
    if (!valueReq)
      continue;

    // If we used a default definition, ignore this requirement.
    if (conformance->usesDefaultDefinition(valueReq))
      continue;

    // Look for the owner of this witness.
    Decl *witnessOwner = nullptr;
    if (auto assocType = dyn_cast<TypeAliasDecl>(req)) {
      // Ignore the 'This' declaration.
      if (assocType == proto->getThis())
        continue;

      auto witnessTy = conformance->getTypeWitness(assocType).Replacement;
      if (auto nameAlias = dyn_cast<NameAliasType>(witnessTy.getPointer())) {
        witnessOwner = getNominalOrExtensionDecl(
                         nameAlias->getDecl()->getDeclContext());
      } else if (auto nominal = witnessTy->getAnyNominal()) {
        witnessOwner = getNominalOrExtensionDecl(nominal->getDeclContext());
      }
    } else {
      auto witness = conformance->getWitness(valueReq).Decl;
      witnessOwner = getNominalOrExtensionDecl(witness->getDeclContext());
    }

    // If the owner was not a declaration, or if we found the same owner
    // twice, there's nothing to update.
    if (!witnessOwner || witnessOwner == owner)
      continue;

    // If the witness owner is not this translation unit, then we don't want
    // to suggest it as a place to hang the explicit conformance.
    if (witnessOwner->getDeclContext()->getParentModule() != &tc.TU)
      continue;

    // We have an owner.

    // If we didn't have an owner, record this as our owner.
    if (!owner) {
      owner = witnessOwner;
      continue;
    }

    // We have two potential owners. Keep the owner that occurs earlier in the
    // translation unit
    // FIXME: < on character pointers is a horrible hack.
    assert(owner != witnessOwner && "Owners cannot match here.");

    if (witnessOwner->getLoc().Value.getPointer() <
          owner->getLoc().Value.getPointer())
      owner = witnessOwner;
  }

  // If we don't have an owner, don't even try to suggest where the explicit
  // conformance should go.
  if (!owner)
    return;

  // Find the inheritance clause and the location where the inheritance clause
  // would be (if it were missing).
  ArrayRef<TypeLoc> inherited;
  SourceLoc inheritedStartLoc;
  if (auto type = dyn_cast<TypeDecl>(owner)) {
    inherited = type->getInherited();
    inheritedStartLoc = type->getLoc();
  } else {
    auto ext = cast<ExtensionDecl>(owner);
    inherited = ext->getInherited();
    inheritedStartLoc = ext->getExtendedTypeLoc().getSourceRange().End;
  }

  // If there is no inheritance clause, introduce a new one with just this
  // conformance...
  if (inherited.empty()) {
    auto insertLoc = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                                inheritedStartLoc);
    tc.diagnose(owner->getLoc(), diag::note_add_conformance,
                proto->getDeclaredType())
      .fixItInsert(insertLoc, " : " + proto->getDeclaredType()->getString());
  } else {
    // ... or tack this conformance onto the end of the existing clause.
    auto insertLoc
      = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                   inherited.back().getSourceRange().End);
    tc.diagnose(inheritedStartLoc, diag::note_add_conformance,
                proto->getDeclaredType())
      .fixItInsert(insertLoc, ", " + proto->getDeclaredType()->getString());
  }

  // FIXME: Update the list of conformances? Update the inheritance clause
  // itself?
}

/// Gather the set of substitutions required to map from the generic form of
/// the given type to the specialized form.
ArrayRef<Substitution> gatherSubstitutions(TypeChecker &tc, Type type) {
  assert(type->isSpecialized() && "Type is not specialized");
  SmallVector<ArrayRef<Substitution>, 2> allSubstitutions;

  while (type) {
    // Record the substitutions in a bound generic type.
    if (auto boundGeneric = type->getAs<BoundGenericType>()) {
      // FIXME: This feels like a hack. We should be able to compute the
      // substitutions ourselves for this.
      tc.validateTypeSimple(type);
      allSubstitutions.push_back(boundGeneric->getSubstitutions());
      type = boundGeneric->getParent();
      continue;
    }

    // Skip to the parent of a nominal type.
    if (auto nominal = type->getAs<NominalType>()) {
      type = nominal->getParent();
      continue;
    }

    llvm_unreachable("Not a nominal or bound generic type");
  }
  assert(!allSubstitutions.empty() && "No substitutions?");

  // If there is only one list of substitutions, return it. There's no
  // need to copy it.
  if (allSubstitutions.size() == 1)
    return allSubstitutions.front();

  SmallVector<Substitution, 4> flatSubstitutions;
  for (auto substitutions : allSubstitutions)
    flatSubstitutions.append(substitutions.begin(), substitutions.end());
  return tc.Context.AllocateCopy(flatSubstitutions);
}

/// Check whether the given archetype conforms to the protocol.
static bool archetypeConformsToProtocol(TypeChecker &tc, Type type,
                                        ArchetypeType *archetype,
                                        ProtocolDecl *protocol,
                                        SourceLoc complainLoc) {
  for (auto ap : archetype->getConformsTo()) {
    if (ap == protocol || ap->inheritsFrom(protocol))
      return true;
  }

  // If we need to complain, do so.
  if (complainLoc.isValid()) {
    // FIXME: Fix-It to add a requirement on the corresponding type
    // parameter?
    tc.diagnose(complainLoc, diag::type_does_not_conform, type,
                protocol->getDeclaredType());
  }

  return false;
}

/// Check whether the given existential type conforms to the protocol.
static bool existentialConformsToProtocol(TypeChecker &tc, Type type,
                                          ProtocolDecl *protocol,
                                          SourceLoc complainLoc) {
  SmallVector<ProtocolDecl *, 4> protocols;
  bool isExistential = type->isExistentialType(protocols);
  assert(isExistential && "Not existential?");
  (void)isExistential;

  for (auto ap : protocols) {
    // If this isn't the protocol we're looking for, continue looking.
    if (ap != protocol && !ap->inheritsFrom(protocol))
      continue;

    // Check whether this protocol conforms to itself.
    llvm::SmallPtrSet<ProtocolDecl *, 4> checking;
    checking.insert(protocol);
    return existentialConformsToItself(tc, type, ap, complainLoc, checking);
  }

  // We didn't find the protocol we were looking for.
  // If we need to complain, do so.
  if (complainLoc.isValid()) {
    tc.diagnose(complainLoc, diag::type_does_not_conform, type,
             protocol->getDeclaredType());
  }
  return false;
}

/// Given a type witness map and a set of substitutions, produce the specialized
/// type witness map by applying the substitutions to each type witness.
static TypeWitnessMap
specializeTypeWitnesses(TypeChecker &tc,
                        const TypeWitnessMap &witnesses,
                        ArrayRef<Substitution> substitutions) {
  // Compute the substitution map, which is needed for substType().
  TypeSubstitutionMap substitutionMap;
  for (const auto &substitution : substitutions) {
    substitutionMap[substitution.Archetype] = substitution.Replacement;
  }

  // Substitute into each of the type witnesses.
  TypeWitnessMap result;
  for (const auto &genericWitness : witnesses) {
    // Substitute into the type witness to produce the type witness for
    // the specialized type.
    auto specializedType
      = tc.substType(genericWitness.second.Replacement, substitutionMap);

    // If the type witness was unchanged, just copy it directly.
    if (specializedType.getPointer() ==
        genericWitness.second.Replacement.getPointer()) {
      result.insert(genericWitness);
      continue;
    }

    // Gather the conformances for the type witness. These should never fail.
    SmallVector<ProtocolConformance *, 4> conformances;
    auto archetype = genericWitness.second.Archetype;
    for (auto proto : archetype->getConformsTo()) {
      ProtocolConformance *conformance = nullptr;
      bool conforms = tc.conformsToProtocol(specializedType, proto,
                                            &conformance);
      assert(conforms && "Conformance specialization should not fail");
      (void)conforms;

      conformances.push_back(conformance);
    }

    result[genericWitness.first]
      = Substitution{archetype, specializedType,
                     tc.Context.AllocateCopy(conformances) };
  }

  return result;
}

bool TypeChecker::conformsToProtocol(Type T, ProtocolDecl *Proto,
                                     ProtocolConformance **Conformance,
                                     SourceLoc ComplainLoc, 
                                     Decl *ExplicitConformance) {
  if (Conformance)
    *Conformance = nullptr;

  // If we have an archetype, check whether this archetype's requirements
  // include this protocol (or something that inherits from it).
  if (auto Archetype = T->getAs<ArchetypeType>())
    return archetypeConformsToProtocol(*this, T, Archetype, Proto, ComplainLoc);

  // If we have an existential type, check whether this type includes this
  // protocol we're looking for (or something that inherits from it).
  if (T->isExistentialType())
    return existentialConformsToProtocol(*this, T, Proto, ComplainLoc);

  // Check whether we have already cached an answer to this query.
  ASTContext::ConformsToMap::key_type Key(T->getCanonicalType(), Proto);
  ASTContext::ConformsToMap::iterator Known = Context.ConformsTo.find(Key);
  if (Known != Context.ConformsTo.end()) {
    // If we conform, set the conformance and return true.
    if (Known->second.getInt()) {
      if (Conformance)
        *Conformance = Known->second.getPointer();

      return true;
    }

    // If we're just checking for conformance, we already know the answer.
    if (!ExplicitConformance) {
      // Check whether we know we implicitly conform...
      if (Known->second.getPointer()) {
        // We're not allowed to complain; fail.
        if (ComplainLoc.isInvalid())
          return false;

        // Complain about explicit conformance and continue as if the user
        // had written the explicit conformance.
        suggestExplicitConformance(*this, ComplainLoc, T,
                                   Known->second.getPointer());
        return true;
      }

      // If we need to complain, do so.
      if (ComplainLoc.isValid()) {
        diagnose(ComplainLoc, diag::type_does_not_conform, T,
                 Proto->getDeclaredType());
      }

      return false;
    }

    // For explicit conformance, force the check again.
    // FIXME: Detect duplicates here?
    Context.ConformsTo.erase(Known);
  }

  // If we're checking for conformance (rather than stating it),
  // look for the explicit declaration of conformance in the list of protocols.
  if (!ExplicitConformance) {
    // Look through the metatype.
    // FIXME: This is a hack to work around bugs in the solver.
    auto instanceT = T;
    if (auto metaT = T->getAs<MetaTypeType>()) {
      instanceT = metaT->getInstanceType();
    }

    // Only nominal types conform to protocols.
    auto nominal = instanceT->getAnyNominal();
    if (!nominal) {
      // If we need to complain, do so.
      if (ComplainLoc.isValid()) {
        diagnose(ComplainLoc, diag::type_does_not_conform, T,
                 Proto->getDeclaredType());
      }

      return false;
    }

    // Walk the nominal type, its extensions, superclasses, and so on.
    llvm::SmallPtrSet<ProtocolDecl *, 4> visitedProtocols;
    SmallVector<std::tuple<NominalTypeDecl *, NominalTypeDecl *, Decl *>, 4>
      stack;
    NominalTypeDecl *owningNominal = nullptr;

    // Local function that checks for our protocol in the given array of
    // protocols.
    auto isProtocolInList = [&](NominalTypeDecl *currentNominal,
                                Decl *currentOwner,
                                ArrayRef<ProtocolDecl *> protocols) -> bool {
      for (auto testProto : protocols) {
        if (testProto == Proto) {
          owningNominal = currentNominal;
          ExplicitConformance = currentOwner;
          return true;
        }

        if (visitedProtocols.insert(testProto))
          stack.push_back({testProto, currentNominal, currentOwner});
      }

      return false;
    };

    // Walk the stack of types.
    stack.push_back({nominal, nominal, nominal});
    while (!stack.empty()) {
      NominalTypeDecl *current;
      NominalTypeDecl *currentNominal;
      Decl *currentOwner;
      std::tie(current, currentNominal, currentOwner) = stack.back();
      stack.pop_back();

      // Visit the superclass of a class.
      if (auto classDecl = dyn_cast<ClassDecl>(current)) {
        if (auto superclassTy = classDecl->getSuperclass()) {
          auto nominal = superclassTy->getAnyNominal();
          stack.push_back({nominal, nominal, nominal});
        }
      }

      // Visit the protocols this type conforms to directly.
      if (isProtocolInList(currentNominal, currentOwner,
                           getDirectConformsTo(current)))
        break;

      // Visit the extensions of this type.
      for (auto ext : current->getExtensions()) {
        if (isProtocolInList(currentNominal, ext, getDirectConformsTo(ext)))
          break;
      }
    }

    // If we did not find explicit conformance, we're done.
    if (!ExplicitConformance) {
      // If the type has a type variable, there's nothing to record. Just
      // report failure.
      if (T->hasTypeVariable()) {
        return false;
      }

      // Cache the failure
      Context.ConformsTo[Key] = ConformanceEntry(nullptr, false);

      // Check whether the type *implicitly* conforms to the protocol.
      if (auto *result = checkConformsToProtocol(*this, T, Proto, nullptr,
                                                 SourceLoc())) {
        // Success! Record the conformance in the cache.
        Context.ConformsTo[Key].setPointer(result);

        if (Conformance)
          *Conformance = result;

        // If we can't complain about this, just return now.
        if (ComplainLoc.isInvalid()) {
          return false;
        }

        // Suggest the addition of the explicit conformance.
        suggestExplicitConformance(*this, ComplainLoc, T, result);
        return true;
      }

      if (ComplainLoc.isValid()) {
        diagnose(ComplainLoc, diag::type_does_not_conform,
                 T, Proto->getDeclaredType());
      }

      return false;
    }

    // We found explicit conformance.
    assert(!isa<ProtocolDecl>(ExplicitConformance) &&
           "Cannot get a protocol here");

    // If we don't need to compute the conformance, we're done.
    if (!Conformance) {
      return true;
    }

    // Compute the actual conformance below.

    // If the nominal type in which we found the conformance is not the same
    // as the type we asked for, it's an inherited type.
    if (owningNominal != nominal) {
      // Find the superclass type
      ProtocolConformance *inheritedConformance = nullptr;
      Type superclassTy = getSuperClassOf(T);
      while (superclassTy->getAnyNominal() != owningNominal)
        superclassTy = getSuperClassOf(superclassTy);

      // Compute the conformance for the inherited type.
      bool conforms = conformsToProtocol(superclassTy, Proto,
                                         &inheritedConformance);
      assert(conforms && "Superclass does not conform but should?");
      (void)conforms;
      assert(inheritedConformance &&
             "We should have found an inherited conformance");

      // Create the inherited conformance entry.
      Context.ConformsTo[Key] = ConformanceEntry(nullptr, false);
      auto result = Context.getInheritedConformance(T, inheritedConformance);
      Context.ConformsTo[Key] = ConformanceEntry(result, true);

      if (Conformance)
        *Conformance = result;
      return true;
    }

    // If the type is specialized, find the conformance for the generic type.
    if (T->isSpecialized()) {
      // Look through the metatype.
      // FIXME: This is a hack to work around bugs in the solver.
      auto instanceT = T;
      if (auto metaT = T->getAs<MetaTypeType>()) {
        instanceT = metaT->getInstanceType();
      }

      // Figure out the type that's explicitly conforming to this protocol.
      Type explicitConformanceType;
      if (auto nominal = dyn_cast<NominalTypeDecl>(ExplicitConformance)) {
        explicitConformanceType = nominal->getDeclaredTypeInContext();
      } else {
        explicitConformanceType = cast<ExtensionDecl>(ExplicitConformance)
                                    ->getDeclaredTypeInContext();
      }

      // If the explicit conformance is associated with a type that is different
      // from the type we're checking, retrieve generic conformance.
      if (!explicitConformanceType->isEqual(instanceT)) {
        // Retrieve the generic conformance.
        ProtocolConformance *genericConformance = nullptr;
        if (!conformsToProtocol(explicitConformanceType, Proto,
                                &genericConformance, ComplainLoc)) {
          // If generic conformance fails, we're done.
          return false;
        }
        assert(genericConformance && "Missing generic conformance?");

        // Gather the substitutions we need to map the generic conformance to
        // the specialized conformance.
        auto substitutions = gatherSubstitutions(*this, instanceT);

        // The type witnesses for the specialized conformance.
        TypeWitnessMap typeWitnesses
          = specializeTypeWitnesses(*this,
                                    genericConformance->getTypeWitnesses(),
                                    substitutions);

        // Create the specialized conformance entry.
        Context.ConformsTo[Key] = ConformanceEntry(nullptr, false);
        auto result
          = Context.getSpecializedConformance(T, genericConformance,
                                              substitutions,
                                              std::move(typeWitnesses));
        Context.ConformsTo[Key] = ConformanceEntry(result, true);

        if (Conformance)
          *Conformance = result;
        return true;
      }
    }

    // Fall through to check conformance in the implicit case.
    // FIXME: This should be factored out better.
  }

  // Assume that the type does not conform to this protocol while checking
  // whether it does in fact conform. This eliminates both infinite recursion
  // (if the protocol hierarchies are circular) as well as tautologies.
  Context.ConformsTo[Key] = ConformanceEntry(nullptr, false);
  auto result = checkConformsToProtocol(*this, T, Proto, ExplicitConformance,
                                        ComplainLoc);
  if (!result)
    return false;

  // Record the conformance we just computed.
  Context.ConformsTo[Key] = ConformanceEntry(result, true);

  if (Conformance)
    *Conformance = result;
  return true;
}


