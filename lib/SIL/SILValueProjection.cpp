//===--- SILValueProjection.cpp -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-value-projection"
#include "swift/SIL/SILValueProjection.h"
#include "swift/SIL/InstructionUtils.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                              Utility Functions
//===----------------------------------------------------------------------===//

static void
removeLSLocations(LSLocationValueMap &Values, LSLocationList &NextLevel) {
  for (auto &X : NextLevel) {
    Values.erase(X);
  }
}

//===----------------------------------------------------------------------===//
//                              SILValue Projection
//===----------------------------------------------------------------------===//

void SILValueProjection::print(SILModule *Mod) {
  llvm::outs() << Base;
  Path.getValue().print(llvm::outs(), *Mod);
}

void
LSValue::expand(SILValue Base, SILModule *M, LSValueList &Vals,
                TypeExpansionAnalysis *TE) {
  for (const auto &P : TE->getTypeExpansion((*Base).getType(), M)) {
    Vals.push_back(LSValue(Base, P.getValue()));
  }
}

void
LSValue::reduceInner(LSLocation &Base, SILModule *M, LSLocationValueMap &Values,
                     SILInstruction *InsertPt) {
  // If this is a class reference type, we have reached end of the type tree.
  if (Base.getType(M).getClassOrBoundGenericClass())
    return;

  // This is a leaf node, we must have a value for it.
  LSLocationList NextLevel;
  Base.getNextLevelLSLocations(NextLevel, M);
  if (NextLevel.empty())
    return;

  // This is not a leaf node, reduce the next level node one by one.
  for (auto &X : NextLevel) {
    LSValue::reduceInner(X, M, Values, InsertPt);
  }

  // This is NOT a leaf node, we need to construct a value for it.
  auto Iter = NextLevel.begin();
  LSValue &FirstVal = Values[*Iter];

  // There is only 1 children node and its value's projection path is not
  // empty, keep stripping it.
  if (NextLevel.size() == 1 && !FirstVal.hasEmptyProjectionPath()) {
    Values[Base] = FirstVal.stripLastLevelProjection();
    // We have a value for the parent, remove all the values for children.
    removeLSLocations(Values, NextLevel);
    return;
  }

  bool HasIdenticalBase = true;
  SILValue FirstBase = FirstVal.getBase();
  for (auto &X : NextLevel) {
    HasIdenticalBase &= (FirstBase == Values[X].getBase());
  }

  // This is NOT a leaf node and it has multiple children, but they have the
  // same value base.
  if (NextLevel.size() > 1 && HasIdenticalBase) {
    if (!FirstVal.hasEmptyProjectionPath()) {
      Values[Base] = FirstVal.stripLastLevelProjection();
      // We have a value for the parent, remove all the values for children.
      removeLSLocations(Values, NextLevel);
      return;
    }
  }

  // In 3 cases do we need aggregation.
  //
  // 1. If there is only 1 child and we cannot strip off any projections,
  //    that means we need to create an aggregation.
  // 
  // 2. There are multiple children and they have the same base, but empty
  //    projection paths.
  //
  // 3. Children have values from different bases, We need to create
  //    extractions and aggregation in this case.
  //
  llvm::SmallVector<SILValue, 8> Vals;
  for (auto &X : NextLevel) {
    Vals.push_back(Values[X].materialize(InsertPt));
  }
  SILBuilder Builder(InsertPt);
  Builder.setCurrentDebugScope(InsertPt->getFunction()->getDebugScope());
  
  // We use an auto-generated SILLocation for now.
  NullablePtr<swift::SILInstruction> AI =
      Projection::createAggFromFirstLevelProjections(
          Builder, RegularLocation::getAutoGeneratedLocation(),
          Base.getType(M).getObjectType(),
          Vals);

  // This is the Value for the current base.
  ProjectionPath P(Base.getType(M));
  Values[Base] = LSValue(SILValue(AI.get()), P);
  removeLSLocations(Values, NextLevel);
}

SILValue
LSValue::reduce(LSLocation &Base, SILModule *M, LSLocationValueMap &Values,
                SILInstruction *InsertPt) {
  LSValue::reduceInner(Base, M, Values, InsertPt);
  // Finally materialize and return the forwarding SILValue.
  return Values.begin()->second.materialize(InsertPt);
}

bool
LSLocation::isMustAliasLSLocation(const LSLocation &RHS, AliasAnalysis *AA) {
  // If the bases are not must-alias, the locations may not alias.
  if (!AA->isMustAlias(Base, RHS.getBase()))
    return false;
  // If projection paths are different, then the locations cannot alias.
  if (!hasIdenticalProjectionPath(RHS))
    return false;
  // Must-alias base and identical projection path. Same object!.
  return true;
}

bool
LSLocation::isMayAliasLSLocation(const LSLocation &RHS, AliasAnalysis *AA) {
  // If the bases do not alias, then the locations cannot alias.
  if (AA->isNoAlias(Base, RHS.getBase()))
    return false;
  // If one projection path is a prefix of another, then the locations
  // could alias.
  if (hasNonEmptySymmetricPathDifference(RHS))
    return false;
  // We can not prove the 2 locations do not alias.
  return true;
}

void
LSLocation::getNextLevelLSLocations(LSLocationList &Locs, SILModule *Mod) {
  SILType Ty = getType(Mod);
  llvm::SmallVector<Projection, 8> Out;
  Projection::getFirstLevelProjections(Ty, *Mod, Out);
  for (auto &X : Out) {
    ProjectionPath P((*Base).getType());
    P.append(Path.getValue());
    P.append(X);
    Locs.push_back(LSLocation(Base, P));
  }
}

void
LSLocation::expand(LSLocation Base, SILModule *M, LSLocationList &Locs,
                   TypeExpansionAnalysis *TE) {
  const ProjectionPath &BasePath = Base.getPath().getValue();
  for (const auto &P : TE->getTypeExpansion(Base.getType(M), M)) {
    Locs.push_back(LSLocation(Base.getBase(), BasePath, P.getValue()));
  }
}

bool
LSLocation::reduce(LSLocation Base, SILModule *M, LSLocationSet &Locs) {
  // If this is a class reference type, we have reached end of the type tree.
  if (Base.getType(M).getClassOrBoundGenericClass())
    return Locs.find(Base) != Locs.end();

  // This is a leaf node.
  LSLocationList NextLevel;
  Base.getNextLevelLSLocations(NextLevel, M);
  if (NextLevel.empty())
    return Locs.find(Base) != Locs.end();

  // This is not a leaf node, try to find whether all its children are alive.
  bool Alive = true;
  for (auto &X : NextLevel) {
    Alive &= LSLocation::reduce(X, M, Locs);
  }

  // All next level locations are alive, create the new aggregated location.
  if (Alive) {
    for (auto &X : NextLevel)
      Locs.erase(X);
    Locs.insert(Base);
  }
  return Alive;
}

void
LSLocation::enumerateLSLocation(SILModule *M, SILValue Mem,
                                std::vector<LSLocation> &Locations,
                                LSLocationIndexMap &IndexMap,
                                LSLocationBaseMap &BaseMap,
                                TypeExpansionAnalysis *TypeCache) {
  // We have processed this SILValue before.
  if (BaseMap.find(Mem) != BaseMap.end())
    return;

  // Construct a Location to represent the memory written by this instruction.
  SILValue UO = getUnderlyingObject(Mem);
  LSLocation L(UO, ProjectionPath::getProjectionPath(UO, Mem));

  // If we can't figure out the Base or Projection Path for the memory location,
  // simply ignore it for now.
  if (!L.isValid())
    return;

  // Record the SILValue to location mapping.
  BaseMap[Mem] = L; 

  // Expand the given Mem into individual fields and add them to the
  // locationvault.
  LSLocationList Locs;
  LSLocation::expand(L, M, Locs, TypeCache);
  for (auto &Loc : Locs) {
    if (IndexMap.find(Loc) != IndexMap.end())
      continue;
    IndexMap[Loc] = Locations.size();
    Locations.push_back(Loc);
  }
}

void
LSLocation::enumerateLSLocations(SILFunction &F,
                                 std::vector<LSLocation> &Locations,
                                 LSLocationIndexMap &IndexMap,
                                 LSLocationBaseMap &BaseMap,
                                 TypeExpansionAnalysis *TypeCache,
                                 std::pair<int, int> &LSCount) {
  // Enumerate all locations accessed by the loads or stores.
  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        enumerateLSLocation(&I.getModule(), LI->getOperand(), Locations,
                            IndexMap, BaseMap, TypeCache);
        ++LSCount.first;
        continue;
      }
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        enumerateLSLocation(&I.getModule(), SI->getDest(), Locations,
                            IndexMap, BaseMap, TypeCache);
        ++LSCount.second;
        continue;
      }
    }
  }
}
