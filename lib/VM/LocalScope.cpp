/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/LocalScope.h"

#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Runtime-inline.h"

#include "llvh/Support/Debug.h"
#define DEBUG_TYPE "serialize"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
// class LocalScope

void LocalScopeBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<LocalScope>());
  ObjectBuildMeta(cell, mb);
}

//===----------------------------------------------------------------------===//
// class StaticScope

const ObjectVTable StaticScope::vt{
    VTable(CellKind::StaticScopeKind, cellSize<StaticScope>()),
    JSObject::_getOwnIndexedRangeImpl,
    JSObject::_haveOwnIndexedImpl,
    JSObject::_getOwnIndexedPropertyFlagsImpl,
    JSObject::_getOwnIndexedImpl,
    JSObject::_setOwnIndexedImpl,
    JSObject::_deleteOwnIndexedImpl,
    JSObject::_checkAllOwnIndexedImpl,
};

void StaticScopeBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<StaticScope>());
  LocalScopeBuildMeta(cell, mb);
}

#ifdef HERMESVM_SERIALIZE
StaticScope::StaticScope(Deserializer &d) : Super(d, &vt.base) {}

void StaticScopeSerialize(Serializer &s, const GCCell *cell) {
  DynamicScope::serializeObjectImpl(
      s, cell, JSObject::numOverlapSlots<StaticScope>());
}

void StaticScopeDeserialize(Deserializer &d, CellKind kind) {
  assert(kind == CellKind::StaticScopeKind && "Expected StaticScope");
  auto *cell = d.getRuntime()->makeAFixed<StaticScope>(d);
  d.endObject(cell);
}
#endif

PseudoHandle<StaticScope> StaticScope::create(
    Runtime *runtime,
    Handle<JSObject> parent) {
  auto *cell = runtime->makeAFixed<StaticScope>(
      runtime,
      parent,
      runtime->getHiddenClassForPrototype(
          *parent, numOverlapSlots<StaticScope>() + ANONYMOUS_PROPERTY_SLOTS));
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

//===----------------------------------------------------------------------===//
// class DynamicScope

const ObjectVTable DynamicScope::vt{
    VTable(CellKind::DynamicScopeKind, cellSize<DynamicScope>()),
    JSObject::_getOwnIndexedRangeImpl,
    JSObject::_haveOwnIndexedImpl,
    JSObject::_getOwnIndexedPropertyFlagsImpl,
    JSObject::_getOwnIndexedImpl,
    JSObject::_setOwnIndexedImpl,
    JSObject::_deleteOwnIndexedImpl,
    JSObject::_checkAllOwnIndexedImpl,
};

void DynamicScopeBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<DynamicScope>());
  ObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const DynamicScope *>(cell);
  mb.addField("withObject", &self->withObject_);
}

#ifdef HERMESVM_SERIALIZE
DynamicScope::DynamicScope(Deserializer &d) : Super(d, &vt.base) {}

void DynamicScopeSerialize(Serializer &s, const GCCell *cell) {
  JSObject::serializeObjectImpl(
      s, cell, JSObject::numOverlapSlots<DynamicScope>());
  const auto *self = vmcast<const DynamicScope>(cell);
  s.writeRelocation(self->withObject_.get(s.getRuntime()));
  s.endObject(cell);
}

void DynamicScopeDeserialize(Deserializer &d, CellKind kind) {
  assert(kind == CellKind::DynamicScopeKind && "Expected DynamicScope");
  auto *cell = d.getRuntime()->makeAFixed<DynamicScope>(d);
  auto *self = vmcast<DynamicScope>(cell);
  d.readRelocation(&self->withObject_, RelocationKind::GCPointer);
  d.endObject(cell);
}
#endif

PseudoHandle<DynamicScope> DynamicScope::create(
    Runtime *runtime,
    Handle<JSObject> parent,
    Handle<JSObject> withObject) {
  auto *cell = runtime->makeAFixed<DynamicScope>(
      runtime,
      parent,
      runtime->getHiddenClassForPrototype(
          *parent, numOverlapSlots<DynamicScope>() + ANONYMOUS_PROPERTY_SLOTS),
      withObject);
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

} // namespace vm
} // namespace hermes
