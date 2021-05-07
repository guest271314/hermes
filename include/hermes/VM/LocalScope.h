/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_LOCALSCOPE_H
#define HERMES_VM_LOCALSCOPE_H

#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

/// A JSObject representing a runtime local scope created by the compiler.
class LocalScope : public JSObject {
  using Super = JSObject;

  friend void LocalScopeBuildMeta(const GCCell *cell, Metadata::Builder &mb);

 public:
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::StaticScopeKind ||
        cell->getKind() == CellKind::DynamicScopeKind;
  }

  /// \return the parent scope. It is always valid because all scopes lead to
  ///     the global object.
  JSObject *getParentScope(PointerBase *base) const {
    return parent_.getNonNull(base);
  }

 public:
  using JSObject::JSObject;
};

/// A JSObject representing a static scope, which is a scope created by the
/// compiler with a fixed set of properties.
class StaticScope final : public LocalScope {
  using Super = LocalScope;

  friend void StaticScopeBuildMeta(const GCCell *cell, Metadata::Builder &mb);
#ifdef HERMESVM_SERIALIZE
  friend void StaticScopeSerialize(Serializer &s, const GCCell *cell);
  friend void StaticScopeDeserialize(Deserializer &d, CellKind kind);
#endif

 public:
  static const ObjectVTable vt;

  static PseudoHandle<StaticScope> create(
      Runtime *runtime,
      Handle<JSObject> parent);

  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::StaticScopeKind;
  }

 public:
#ifdef HERMESVM_SERIALIZE
  explicit StaticScope(Deserializer &d);
#endif

  StaticScope(
      Runtime *runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : Super(runtime, &vt.base, *parent, *clazz) {}
};

/// A JSObject representing a dynamic scope, which is a scope for non-strict
/// local eval, or "with". The latter is distinguished by
class DynamicScope final : public LocalScope {
  using Super = LocalScope;

  friend void DynamicScopeBuildMeta(const GCCell *cell, Metadata::Builder &mb);
#ifdef HERMESVM_SERIALIZE
  friend void DynamicScopeSerialize(Serializer &s, const GCCell *cell);
  friend void DynamicScopeDeserialize(Deserializer &d, CellKind kind);
#endif

  GCPointer<JSObject> withObject_;

  static PseudoHandle<DynamicScope> create(
      Runtime *runtime,
      Handle<JSObject> parent,
      Handle<JSObject> withObject);

 public:
  static const ObjectVTable vt;

  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::DynamicScopeKind;
  }

  /// Create a dynamic scope for a "with" statement.
  static PseudoHandle<DynamicScope> createForWith(
      Runtime *runtime,
      Handle<JSObject> parent,
      Handle<JSObject> withObject) {
    assert(withObject.get() && "withObject cannot be null");
    return create(runtime, parent, withObject);
  }

  /// Create a dynamic scope for non-strict local eval.
  static PseudoHandle<DynamicScope> createForEval(
      Runtime *runtime,
      Handle<JSObject> parent) {
    return create(runtime, parent, runtime->makeNullHandle<JSObject>());
  }

  /// \return the "with" object. It may be null if this is an eval() scope.
  JSObject *getWithObject(PointerBase *base) const {
    return withObject_.get(base);
  }

  /// \return true if this scope is created for "with".
  bool isScopeForWith() const {
    return (bool)withObject_;
  }

 public:
#ifdef HERMESVM_SERIALIZE
  explicit DynamicScope(Deserializer &d);
#endif

  DynamicScope(
      Runtime *runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<JSObject> withObject)
      : Super(runtime, &vt.base, *parent, *clazz),
        withObject_(
            runtime,
            *withObject,
            &runtime->getHeap(),
            GCPointerBase::NoBarriers()) {}
};

} // namespace vm
} // namespace hermes

#endif
