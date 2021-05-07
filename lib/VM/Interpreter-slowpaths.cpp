/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "vm"
#include "JSLib/JSLibInternal.h"
#include "hermes/Support/Statistic.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/Interpreter.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/LocalScope.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime-inline.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringPrimitive.h"

#include "Interpreter-internal.h"

using namespace hermes::inst;

HERMES_SLOW_STATISTIC(NumGetDynamic, "NumGetDynamic");
HERMES_SLOW_STATISTIC(
    NumGetDynamicLocalCacheHits,
    "NumGetDynamicLocalCacheHits - number of hits in a local scope");
HERMES_SLOW_STATISTIC(
    NumGetDynamicGlobalCacheHits,
    "NumGetDynamicGlobalCacheHits - number of cache hits in the global scope");

HERMES_SLOW_STATISTIC(NumPutDynamic, "NumPutDynamic");
HERMES_SLOW_STATISTIC(
    NumPutDynamicLocalCacheHits,
    "NumPutDynamicLocalCacheHits - number of hits in a local scope");
HERMES_SLOW_STATISTIC(
    NumPutDynamicGlobalCacheHits,
    "NumPutDynamicGlobalCacheHits - number of cache hits in the global scope");

namespace hermes {
namespace vm {

void Interpreter::saveGenerator(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const Inst *resumeIP) {
  auto *innerFn = vmcast<GeneratorInnerFunction>(FRAME.getCalleeClosure());
  innerFn->saveStack(runtime);
  innerFn->setNextIP(resumeIP);
  innerFn->setState(GeneratorInnerFunction::State::SuspendedYield);
}

static LocalEvalFlags decodeEvalFlags(uint8_t flags) {
  static_assert(kEvalFlagsVersion == 1, "Eval flags version has changed");
  static_assert(
      LocalEvalFlags::kVersion == 1, "LocalEvalFlags version has changed");
  LocalEvalFlags res{};
  if (flags & kEvalFlagStrictMode)
    res.strictMode = true;
  if (flags & kEvalFlagParamYield)
    res.paramYield = true;
  if (flags & kEvalFlagParamAwait)
    res.paramAwait = true;
  return res;
}

ExecutionStatus Interpreter::caseDirectEval(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  auto *result = &O1REG(DirectEval);
  auto *input = &O4REG(DirectEval);

  GCScopeMarkerRAII gcMarker{runtime};

  // Check to see if global eval() has been overridden, in which case call it as
  // as normal function.
  auto global = runtime->getGlobal();
  auto existingEval = global->getNamed_RJS(
      global, runtime, Predefined::getSymbolID(Predefined::eval));
  if (LLVM_UNLIKELY(existingEval == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto *nativeExistingEval = dyn_vmcast<NativeFunction>(existingEval->get());
  if (LLVM_UNLIKELY(
          !nativeExistingEval ||
          nativeExistingEval->getFunctionPtr() != hermes::vm::eval)) {
    if (auto *existingEvalCallable =
            dyn_vmcast<Callable>(existingEval->get())) {
      auto evalRes = existingEvalCallable->executeCall1(
          runtime->makeHandle<Callable>(existingEvalCallable),
          runtime,
          Runtime::getUndefinedValue(),
          *input);
      if (LLVM_UNLIKELY(evalRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      *result = evalRes->get();
      evalRes->invalidate();
      return ExecutionStatus::RETURNED;
    }
    return runtime->raiseTypeErrorForValue(
        runtime->makeHandle(std::move(*existingEval)), " is not a function");
  }

  if (!input->isString()) {
    *result = *input;
    return ExecutionStatus::RETURNED;
  }

  auto cr = vm::evalInScope(
      runtime,
      Handle<StringPrimitive>::vmcast(input),
      Handle<JSObject>::vmcast(&O2REG(DirectEval)),
      Handle<>(&O3REG(DirectEval)),
      decodeEvalFlags(ip->iDirectEval.op5),
      false);
  if (cr == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  *result = *cr;
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::casePutOwnByVal(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  return JSObject::defineOwnComputed(
             Handle<JSObject>::vmcast(&O1REG(PutOwnByVal)),
             runtime,
             Handle<>(&O3REG(PutOwnByVal)),
             ip->iPutOwnByVal.op4
                 ? DefinePropertyFlags::getDefaultNewPropertyFlags()
                 : DefinePropertyFlags::getNewNonEnumerableFlags(),
             Handle<>(&O2REG(PutOwnByVal)))
      .getStatus();
}

ExecutionStatus Interpreter::casePutOwnGetterSetterByVal(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  DefinePropertyFlags dpFlags{};
  dpFlags.setConfigurable = 1;
  dpFlags.configurable = 1;
  dpFlags.setEnumerable = 1;
  dpFlags.enumerable = ip->iPutOwnGetterSetterByVal.op5;

  MutableHandle<Callable> getter(runtime);
  MutableHandle<Callable> setter(runtime);
  if (LLVM_LIKELY(!O3REG(PutOwnGetterSetterByVal).isUndefined())) {
    dpFlags.setGetter = 1;
    getter = vmcast<Callable>(O3REG(PutOwnGetterSetterByVal));
  }
  if (LLVM_LIKELY(!O4REG(PutOwnGetterSetterByVal).isUndefined())) {
    dpFlags.setSetter = 1;
    setter = vmcast<Callable>(O4REG(PutOwnGetterSetterByVal));
  }
  assert(
      (dpFlags.setSetter || dpFlags.setGetter) &&
      "No accessor set in PutOwnGetterSetterByVal");

  auto res = PropertyAccessor::create(runtime, getter, setter);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  auto accessor = runtime->makeHandle<PropertyAccessor>(*res);

  return JSObject::defineOwnComputed(
             Handle<JSObject>::vmcast(&O1REG(PutOwnGetterSetterByVal)),
             runtime,
             Handle<>(&O2REG(PutOwnGetterSetterByVal)),
             dpFlags,
             accessor)
      .getStatus();
}

ExecutionStatus Interpreter::caseIteratorBegin(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  if (LLVM_LIKELY(vmisa<JSArray>(O2REG(IteratorBegin)))) {
    // Attempt to get the fast path for array iteration.
    NamedPropertyDescriptor desc;
    JSObject *propObj = JSObject::getNamedDescriptorPredefined(
        Handle<JSArray>::vmcast(&O2REG(IteratorBegin)),
        runtime,
        Predefined::SymbolIterator,
        desc);
    if (LLVM_LIKELY(propObj)) {
      auto slotValueRes = JSObject::getNamedSlotValue(
          createPseudoHandle(propObj), runtime, desc);
      if (LLVM_UNLIKELY(slotValueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      PseudoHandle<> slotValue = std::move(*slotValueRes);
      if (LLVM_LIKELY(
              slotValue->getRaw() == runtime->arrayPrototypeValues.getRaw())) {
        O1REG(IteratorBegin) = HermesValue::encodeNumberValue(0);
        return ExecutionStatus::RETURNED;
      }
    }
  }
  GCScopeMarkerRAII marker{runtime};
  CallResult<IteratorRecord> iterRecord =
      getIterator(runtime, Handle<>(&O2REG(IteratorBegin)));
  if (LLVM_UNLIKELY(iterRecord == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  O1REG(IteratorBegin) = iterRecord->iterator.getHermesValue();
  O2REG(IteratorBegin) = iterRecord->nextMethod.getHermesValue();
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseIteratorNext(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip) {
  if (LLVM_LIKELY(O2REG(IteratorNext).isNumber())) {
    JSArray::size_type i =
        O2REG(IteratorNext).getNumberAs<JSArray::size_type>();
    if (i >=
        JSArray::getLength(vmcast<JSArray>(O3REG(IteratorNext)), runtime)) {
      // Finished iterating the array, stop.
      O2REG(IteratorNext) = HermesValue::encodeUndefinedValue();
      O1REG(IteratorNext) = HermesValue::encodeUndefinedValue();
      return ExecutionStatus::RETURNED;
    }
    Handle<JSArray> arr = Handle<JSArray>::vmcast(&O3REG(IteratorNext));
    {
      // Fast path: look up the property in indexed storage.
      // Runs when there is no hole and a regular non-accessor property exists
      // at the current index, because those are the only properties stored
      // in indexed storage.
      // If there is another kind of property we have to call getComputed_RJS.
      // No need to check the fastIndexProperties flag because the indexed
      // storage would be deleted and at() would return empty in that case.
      NoAllocScope noAlloc{runtime};
      HermesValue value = arr->at(runtime, i);
      if (LLVM_LIKELY(!value.isEmpty())) {
        O1REG(IteratorNext) = value;
        O2REG(IteratorNext) = HermesValue::encodeNumberValue(i + 1);
        return ExecutionStatus::RETURNED;
      }
    }
    // Slow path, just run the full getComputedPropertyValue_RJS path.
    GCScopeMarkerRAII marker{runtime};
    Handle<> idxHandle{&O2REG(IteratorNext)};
    CallResult<PseudoHandle<>> valueRes =
        JSObject::getComputed_RJS(arr, runtime, idxHandle);
    if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O1REG(IteratorNext) = valueRes->get();
    O2REG(IteratorNext) = HermesValue::encodeNumberValue(i + 1);
    return ExecutionStatus::RETURNED;
  }
  if (LLVM_UNLIKELY(O2REG(IteratorNext).isUndefined())) {
    // In all current use cases of IteratorNext, we check and branch away
    // from IteratorNext in the case that iterStorage was set to undefined
    // (which indicates completion of iteration).
    // If we introduce a use case which allows calling IteratorNext,
    // then this assert can be removed. For now, this branch just returned
    // undefined in NDEBUG mode.
    assert(false && "IteratorNext called on completed iterator");
    O1REG(IteratorNext) = HermesValue::encodeUndefinedValue();
    return ExecutionStatus::RETURNED;
  }

  GCScopeMarkerRAII marker{runtime};

  IteratorRecord iterRecord{
      Handle<JSObject>::vmcast(&O2REG(IteratorNext)),
      Handle<Callable>::vmcast(&O3REG(IteratorNext))};

  CallResult<PseudoHandle<JSObject>> resultObjRes =
      iteratorNext(runtime, iterRecord, llvh::None);
  if (LLVM_UNLIKELY(resultObjRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> resultObj = runtime->makeHandle(std::move(*resultObjRes));
  CallResult<PseudoHandle<>> doneRes = JSObject::getNamed_RJS(
      resultObj, runtime, Predefined::getSymbolID(Predefined::done));
  if (LLVM_UNLIKELY(doneRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (toBoolean(doneRes->get())) {
    // Done with iteration. Clear the iterator so that subsequent
    // instructions do not call next() or return().
    O2REG(IteratorNext) = HermesValue::encodeUndefinedValue();
    O1REG(IteratorNext) = HermesValue::encodeUndefinedValue();
  } else {
    // Not done iterating, so get the `value` property and store it
    // as the result.
    CallResult<PseudoHandle<>> propRes = JSObject::getNamed_RJS(
        resultObj, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O1REG(IteratorNext) = propRes->get();
    propRes->invalidate();
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseGetPNameList(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  if (O2REG(GetPNameList).isUndefined() || O2REG(GetPNameList).isNull()) {
    // Set the iterator to be undefined value.
    O1REG(GetPNameList) = HermesValue::encodeUndefinedValue();
    return ExecutionStatus::RETURNED;
  }

  // Convert to object and store it back to the register.
  auto res = toObject(runtime, Handle<>(&O2REG(GetPNameList)));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  O2REG(GetPNameList) = res.getValue();

  auto obj = runtime->makeMutableHandle(vmcast<JSObject>(res.getValue()));
  uint32_t beginIndex;
  uint32_t endIndex;
  auto cr = getForInPropertyNames(runtime, obj, beginIndex, endIndex);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto arr = *cr;
  O1REG(GetPNameList) = arr.getHermesValue();
  O3REG(GetPNameList) = HermesValue::encodeNumberValue(beginIndex);
  O4REG(GetPNameList) = HermesValue::encodeNumberValue(endIndex);
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::implCallBuiltin(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    CodeBlock *curCodeBlock,
    uint32_t op3) {
  const Inst *ip = runtime->getCurrentIP();
  uint8_t methodIndex = ip->iCallBuiltin.op2;
  Callable *callable = runtime->getBuiltinCallable(methodIndex);
  assert(
      isNativeBuiltin(methodIndex) &&
      "CallBuiltin must take a native builtin.");
  NativeFunction *nf = vmcast<NativeFunction>(callable);

  auto newFrame = StackFramePtr::initFrame(
      runtime->stackPointer_, FRAME, ip, curCodeBlock, op3 - 1, nf, false);
  // "thisArg" is implicitly assumed to "undefined".
  newFrame.getThisArgRef() = HermesValue::encodeUndefinedValue();

  SLOW_DEBUG(dumpCallArguments(llvh::dbgs(), runtime, newFrame));

  auto resPH = NativeFunction::_nativeCall(nf, runtime);
  if (LLVM_UNLIKELY(resPH == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  O1REG(CallBuiltin) = std::move(resPH->get());
  SLOW_DEBUG(
      llvh::dbgs() << "native return value r" << (unsigned)ip->iCallBuiltin.op1
                   << "=" << DumpHermesValue(O1REG(CallBuiltin)) << "\n");
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::caseNewDynamicScope(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const Inst *ip) {
  auto parent = O2REG(NewDynamicScope).isNull()
      ? runtime->getGlobal()
      : Handle<JSObject>::vmcast(&O2REG(NewDynamicScope));

  if (!O3REG(NewDynamicScope).isEmpty()) {
    auto cr = toObject(runtime, Handle<>(&O3REG(NewDynamicScope)));
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    O1REG(NewDynamicScope) =
        DynamicScope::createForWith(
            runtime, parent, runtime->makeHandle<JSObject>(cr.getValue()))
            .getHermesValue();
  } else {
    O1REG(NewDynamicScope) =
        DynamicScope::createForEval(runtime, parent).getHermesValue();
  }
  return ExecutionStatus::RETURNED;
}

//===----------------------------------------------------------------------===//
// Dynamic variable access.
//
// Variables are stored in a chain of objects (lexical scopes) linked through
// the parent field.
// The top-most parent is always the global object, which is treated specially.
// Variables in all objects except the global are created only by us, so they
// are always "regular" variables.
//
// Conceptually We look for each variable in the object chain in a manner
// similar to regular property access. However we can't quite use the normal
// JSObject methods because the behavior is not exactly the same (for example,
// when writing, we must update the variable even if it is in a parent scope),
// and also because of the requirements of our caching.
//
// Most non-global scopes are "static", meaning the set of variables declared in
// a scope doesn't change. There are two exceptions:
// 1. "with". A "with" scope can contain any variables that may change at any
//    time, so it is considered "dynamic".
// 2. Non-strict local eval(). The non-strict local eval() can modify its
//    surrounding scope by adding variables to it. So, it is also considered
//    "dynamic".
//
// There are three cases that we need to handle:
// a) The best case is when we locate the variable in a non-global scope, and
//    all scopes under it are fully static. In that case we record the "depth"
//    (how many levels of scopes up to go), and the index of the property slot
//    of the variable. Since all scopes are static, these values are guaranteed
//    to never change.
// b) We pass through dynamic scopes before we have located the variable or
//    reached the global scope. That means that we cannot cache anything,
//    because any new variable can appear in the dynamic scopes.
// c) We locate the variable in the global scope. In that case we store a depth
//    of -1 to indicate "global". If the variable is cacheable, we also store
//    the hidden class and the property slot.
//
// Caching Scheme
// ==============
// We are using the global cache. Each cache entry has the following fields:
// - key : unique identifies the instruction.
// - value1i32 : the scope depth relative to the current. -1 means the global
//    scope. In that case the rest of the cache data is unused.
// - value2u32 : slot index of the property, if not in global scope.
// - weakValue : not used.

/// This is a value stored in a the cache entry, indicating the global object.
static int32_t kCacheDepthGlobal = -1;

/// Returned as depth by \c findDynamic(), indicating that the variable's scope
/// cannot be cached.
static int32_t kFindNonCacheable = -1;

/// \return true if the result of \c findDynamic() represents a cacheable scope.
static inline bool isFindResCacheable(
    const std::pair<JSObject *, int32_t> &fr) {
  return fr.second >= 0;
}

/// Find a named property in the specified object and its parents.
/// \param desc  the property descriptor is saved there.
/// \return a pair {object, depth} where \c object is the object we found the
///     property in (might be the global object, a with object, etc) and
///     \c depth is the relative depth of the scope relative to the starting
///     scope (with 0 being the starting scope).
///     However, regardless of whether and where the property was found, depth
///     will be returned as kFindNonCacheable if we encountered a dynamic scope
///     along the way.
static std::pair<JSObject *, int32_t> findDynamic(
    Handle<JSObject> objHandle,
    Runtime *runtime,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  // Set to true if we encountered a dynamic scope.
  bool dynamic = false;
  int32_t depth = 0;
  MutableHandle<JSObject> curObj(runtime, *objHandle);
  // We may need this handle if we encounter a "with" statement, but we will
  // create it lazily.
  llvh::Optional<MutableHandle<JSObject>> withObjHandle;

  for (;;) {
    // Is this a dynamic scope?
    if (LLVM_UNLIKELY(vmisa<DynamicScope>(*curObj))) {
      // Record that whatever we find will not be cacheable.
      dynamic = true;
      // If this is a "with" statement scope, search in the object.
      if (auto *withObj =
              vmcast<DynamicScope>(*curObj)->getWithObject(runtime)) {
        if (!withObjHandle.hasValue())
          withObjHandle.emplace(runtime);
        withObjHandle.getValue() = withObj;
        if (auto *found = JSObject::getNamedDescriptor(
                *withObjHandle, runtime, name, desc)) {
          return {found, kFindNonCacheable};
        }
      }
    }

    // This is a JSObject created by us (the compiler).
    auto findRes = JSObject::findProperty(curObj, runtime, name, desc);
    if (findRes)
      return {*curObj, dynamic ? kFindNonCacheable : depth};

    if (!vmisa<LocalScope>(*curObj)) {
      // We have arrived at the global scope.
      assert(
          curObj.get() == runtime->getGlobal().get() &&
          "The global object must always be the top-most scope");
      break;
    }

    curObj = vmcast<LocalScope>(*curObj)->getParentScope(runtime);
    ++depth;
  }

  return {nullptr, dynamic ? kFindNonCacheable : 0};
}

/// Raise a missing variable exception.
static ExecutionStatus raiseMissingVar(Runtime *runtime, SymbolID name) {
  return runtime->raiseReferenceError(
      TwineChar16("Variable ") +
      runtime->getIdentifierTable().getStringViewForDev(runtime, name) +
      "' doesn't exist");
}

/// Raise an uninitialized variable exception.
static ExecutionStatus raiseUninitializedVar(Runtime *runtime, SymbolID name) {
  return runtime->raiseReferenceError(
      TwineChar16("Variable ") +
      runtime->getIdentifierTable().getStringViewForDev(runtime, name) +
      "' is not initialized");
}

/// Raise a constant variable exception.
static ExecutionStatus raiseConstVar(Runtime *runtime, SymbolID name) {
  return runtime->raiseTypeError(
      TwineChar16("Assignment to constant variable ") +
      runtime->getIdentifierTable().getStringViewForDev(runtime, name));
}

ExecutionStatus Interpreter::caseGetDynamic(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip,
    CodeBlock *curCodeBlock) {
  ++NumGetDynamic;

  // Did we previously populate this cache entry? If not, go to the slow path.
  auto *cache = runtime->getGlobalCache().getCacheEntry(ip);
  if (LLVM_UNLIKELY(!GlobalCache::checkKey(
          cache,
          ip,
          curCodeBlock->getRuntimeModule()->calcUniqueFunctionID(
              curCodeBlock->getFunctionID())))) {
    return _caseGetDynamicSlowPath(runtime, frameRegs, ip, curCodeBlock);
  }

  int32_t depth = GlobalCache::getValue1i32(cache);

  // Is the cached variable in the global scope?
  if (LLVM_UNLIKELY(depth < 0)) {
    ++NumGetDynamicGlobalCacheHits;
    SymbolID name = ID(ip->iGetDynamic.op3);
    auto res = JSObject::getNamed_RJS(
        runtime->getGlobal(),
        runtime,
        name,
        ip->opCode == OpCode::TryGetDynamic ? PropOpFlags().plusMustExist()
                                            : PropOpFlags());
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // Note that we don't need to check for "empty" because this is the
    // global object.
    assert(
        !res->getHermesValue().isEmpty() &&
        "values in the global object cannot be empty");
    O1REG(GetDynamic) = res->getHermesValue();
    return ExecutionStatus::RETURNED;
  }

  ++NumGetDynamicLocalCacheHits;

  // The variable lives in a local scope.
  auto *obj = vmcast<LocalScope>(O2REG(GetDynamic));
  for (; depth; --depth)
    obj = vmcast<LocalScope>(obj->getParentScope(runtime));

  // Use the stored slot.
  HermesValue value = JSObject::getNamedSlotValue(
      obj, runtime, (SlotIndex)GlobalCache::getValue2u32(cache));

  if (LLVM_UNLIKELY(value.isEmpty())) {
    SymbolID name = ID(ip->iGetDynamic.op3);
    return raiseUninitializedVar(runtime, name);
  }

  O1REG(GetDynamic) = value;
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::_caseGetDynamicSlowPath(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip,
    CodeBlock *curCodeBlock) {
  SymbolID name = ID(ip->iGetDynamic.op3);
  // Find the property.
  NamedPropertyDescriptor desc;
  auto findRes = findDynamic(
      Handle<JSObject>::vmcast(&O2REG(GetDynamic)), runtime, name, desc);

  // Does the property exist?
  if (LLVM_UNLIKELY(!findRes.first)) {
    if (ip->opCode == OpCode::TryGetDynamic) {
      return raiseMissingVar(runtime, name);
    } else {
      O1REG(GetDynamic) = HermesValue::encodeUndefinedValue();
      return ExecutionStatus::RETURNED;
    }
  }

  if (isFindResCacheable(findRes)) {
    auto *cache = runtime->getGlobalCache().getCacheEntry(ip);
    auto uniqueFunctionID =
        curCodeBlock->getRuntimeModule()->calcUniqueFunctionID(
            curCodeBlock->getFunctionID());
    // Is it in a local scope?
    if (vmisa<StaticScope>(findRes.first)) {
      // The entry is in a static scope. Make sure that our assumptions are
      // correct.
      assert(
          !desc.flags.accessor &&
          !findRes.first->getClass(runtime)->isDictionaryNoCache() &&
          "Static scopes cannot contain accessors or support deletions");

      GlobalCache::initEntry(
          cache, ip, uniqueFunctionID, findRes.second, desc.slot);
      HermesValue value =
          JSObject::getNamedSlotValue(findRes.first, runtime, desc.slot);

      // Is it uninitialized?
      if (LLVM_UNLIKELY(value.isEmpty()))
        return raiseUninitializedVar(runtime, name);

      O1REG(GetDynamic) = value;
      return ExecutionStatus::RETURNED;
    }

    // If the variable is in global scope, all we cache is the fact that
    // it is there, to avoid scanning the scopes again. But we don't cache
    // anything more - we could cache the global class and slot index, but
    // the complexity is not worth the effort.
    GlobalCache::initEntry(cache, ip, uniqueFunctionID, kCacheDepthGlobal, 0);
  }

  auto objHandle = runtime->makeHandle(findRes.first);
  auto res =
      JSObject::getNamedPropertyValue_RJS(objHandle, runtime, objHandle, desc);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  HermesValue value = res->getHermesValue();

  // Is it uninitialized?
  if (LLVM_UNLIKELY(value.isEmpty()))
    return raiseUninitializedVar(runtime, name);

  O1REG(GetDynamic) = value;
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::casePutDynamic(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip,
    CodeBlock *curCodeBlock) {
  ++NumPutDynamic;

  // Did we previously populate this cache entry? If not, go to the slow path.
  auto *cache = runtime->getGlobalCache().getCacheEntry(ip);
  if (LLVM_LIKELY(!GlobalCache::checkKey(
          cache,
          ip,
          curCodeBlock->getRuntimeModule()->calcUniqueFunctionID(
              curCodeBlock->getFunctionID())))) {
    return _casePutDynamicSlowPath(runtime, frameRegs, ip, curCodeBlock);
  }

  int32_t depth = GlobalCache::getValue1i32(cache);

  // Is the variable in the global scope?
  if (LLVM_UNLIKELY(depth < 0)) {
    ++NumPutDynamicGlobalCacheHits;
    // Store the property in the global object.
    SymbolID name = ID(ip->iPutDynamic.op3);
    auto res = JSObject::putNamed_RJS(
        runtime->getGlobal(),
        runtime,
        name,
        Handle<>(&O2REG(PutDynamic)),
        ip->opCode == inst::OpCode::TryPutDynamic
            ? PropOpFlags().plusThrowOnError().plusMustExist()
            : PropOpFlags());
    return res.getStatus();
  }

  ++NumPutDynamicLocalCacheHits;

  // The variable lives in a local scope.
  auto *obj = vmcast<LocalScope>(O2REG(PutDynamic));
  for (; depth; --depth)
    obj = vmcast<LocalScope>(obj->getParentScope(runtime));

  // Use the stored slot.
  auto slotIndex = (SlotIndex)GlobalCache::getValue2u32(cache);
  HermesValue value = JSObject::getNamedSlotValue(obj, runtime, slotIndex);

  // Is it uninitialized?
  if (LLVM_UNLIKELY(value.isEmpty())) {
    SymbolID name = ID(ip->iPutDynamic.op3);
    return raiseUninitializedVar(runtime, name);
  }

  // Note that the slow path would only have cached a variable if it was
  // writable, so we don't need to check (and we can't anyway).

  JSObject::setNamedSlotValue(obj, runtime, slotIndex, O2REG(PutDynamic));
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Interpreter::_casePutDynamicSlowPath(
    Runtime *runtime,
    PinnedHermesValue *frameRegs,
    const inst::Inst *ip,
    CodeBlock *curCodeBlock) {
  SymbolID name = ID(ip->iPutDynamic.op3);

  // Find the property.
  NamedPropertyDescriptor desc;
  auto findRes = findDynamic(
      Handle<JSObject>::vmcast(&O1REG(GetDynamic)), runtime, name, desc);

  if (LLVM_UNLIKELY(!findRes.first)) {
    // We could not find the property. If we are in "Try" mode, throw.
    // Otherwise, we want to write it to the global scope.
    if (ip->opCode == inst::OpCode::TryPutDynamic)
      return raiseMissingVar(runtime, name);

    // Store a new property in the global object.
    auto res = JSObject::putNamed_RJS(
        runtime->getGlobal(),
        runtime,
        name,
        Handle<>(&O2REG(PutDynamic)),
        PropOpFlags());
    return res.getStatus();
  }

  // Check whether the found scope is cacheable, but also prevent caching
  // of read-only local variables, because the fast path can't check their
  // property flags.
  if (isFindResCacheable(findRes) &&
      (desc.flags.writable || !vmisa<LocalScope>(findRes.first))) {
    auto *cache = runtime->getGlobalCache().getCacheEntry(ip);
    auto uniqueFunctionID =
        curCodeBlock->getRuntimeModule()->calcUniqueFunctionID(
            curCodeBlock->getFunctionID());

    // Is it in a local scope?
    if (vmisa<LocalScope>(findRes.first)) {
      // The entry is in a static scope. Make sure that our assumptions are
      // correct.
      assert(
          !desc.flags.accessor &&
          !findRes.first->getClass(runtime)->isDictionaryNoCache() &&
          "Static scopes cannot contain accessors or support deletions");

      GlobalCache::initEntry(
          cache, ip, uniqueFunctionID, findRes.second, desc.slot);
    } else {
      // If the variable is in global scope, all we cache is the fact that
      // it is there, to avoid scanning the scopes again. But we don't cache
      // anything more - we could cache the global class and slot index, but
      // the complexity is not worth the effort.
      GlobalCache::initEntry(cache, ip, uniqueFunctionID, kCacheDepthGlobal, 0);
    }
  }

  // Stores to local scopes must check for uninitialized.
  // the variable is not writable (meaning it is a const).
  if (vmisa<LocalScope>(findRes.first)) {
    HermesValue value =
        JSObject::getNamedSlotValue(findRes.first, runtime, desc.slot);

    // Is it uninitialized?
    if (LLVM_UNLIKELY(value.isEmpty()))
      return raiseUninitializedVar(runtime, name);
    // Non-writable?
    if (LLVM_UNLIKELY(!desc.flags.writable)) {
      if (desc.flags.throwOnWrite)
        return raiseConstVar(runtime, name);
      return ExecutionStatus::RETURNED;
    }

    JSObject::setNamedSlotValue(
        findRes.first, runtime, desc.slot, O2REG(PutDynamic));
    return ExecutionStatus::RETURNED;
  }

  // Store the property in an object.
  auto res = JSObject::putNamed_RJS(
      runtime->makeHandle(findRes.first),
      runtime,
      name,
      Handle<>(&O2REG(PutDynamic)),
      ip->opCode == inst::OpCode::TryPutDynamic
          ? PropOpFlags().plusThrowOnError()
          : PropOpFlags());
  return res.getStatus();
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE
