/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/AST/SemValidate.h"
#include "hermes/BCGen/HBC/BytecodeStream.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/IR/IR.h"
#include "hermes/IRGen/IRGen.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/Support/SimpleDiagHandler.h"
#include "hermes/Utils/Options.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/StringView.h"
#include "llvh/Support/ConvertUTF.h"
#include "llvh/Support/raw_ostream.h"

#define DEBUG_TYPE "eval"

namespace hermes {
namespace vm {

static hermes::LocalEvalFlags cvtLocalEvalFlags(vm::LocalEvalFlags f) {
  hermes::LocalEvalFlags res{};
  static_assert(
      vm::LocalEvalFlags::kVersion == 1,
      "Version of LocalEvalFlags has changed");
  static_assert(
      vm::LocalEvalFlags::kVersion == hermes::LocalEvalFlags::kVersion,
      "Versions of LocalEvalFlags must match");
  res.strictMode = f.strictMode;
  res.paramYield = f.paramYield;
  res.paramAwait = f.paramAwait;
  return res;
}

CallResult<HermesValue> evalInScope(
    Runtime *runtime,
    llvh::StringRef utf8code,
    Handle<JSObject> scope,
    Handle<> thisArg,
    LocalEvalFlags localEvalFlags,
    bool singleFunction) {
  LLVM_DEBUG(llvh::dbgs() << "EVAL:" << utf8code << "\n");
#ifdef HERMESVM_LEAN
  return runtime->raiseEvalUnsupported(utf8code);
#else
  if (!runtime->enableEval) {
    return runtime->raiseEvalUnsupported(utf8code);
  }

  hbc::CompileFlags compileFlags;
  compileFlags.evalMode = !singleFunction;
  compileFlags.localEvalFlags = cvtLocalEvalFlags(localEvalFlags);
  compileFlags.includeLibHermes = false;
  compileFlags.optimize = runtime->optimizedEval;
  compileFlags.verifyIR = runtime->verifyEvalIR;
  compileFlags.emitAsyncBreakCheck = runtime->asyncBreakCheckInEval;
  compileFlags.lazy =
      utf8code.size() >= compileFlags.preemptiveFileCompilationThreshold;
  compileFlags.allowFunctionToStringWithRuntimeSource =
      runtime->getAllowFunctionToStringWithRuntimeSource();
#ifdef HERMES_ENABLE_DEBUGGER
  // Required to allow stepping and examining local variables in eval'd code
  compileFlags.debug = true;
#endif
  LLVM_DEBUG(compileFlags.dumpIR = true);

  std::unique_ptr<hbc::BCProviderFromSrc> bytecode;
  {
    std::unique_ptr<hermes::Buffer> buffer;
    if (compileFlags.lazy ||
        compileFlags.allowFunctionToStringWithRuntimeSource) {
      buffer.reset(new hermes::OwnedMemoryBuffer(
          llvh::MemoryBuffer::getMemBufferCopy(utf8code)));
    } else {
      buffer.reset(new hermes::OwnedMemoryBuffer(
          llvh::MemoryBuffer::getMemBuffer(utf8code)));
    }

    auto bytecode_err = hbc::BCProviderFromSrc::createBCProviderFromSrc(
        std::move(buffer), "JavaScript", nullptr, compileFlags);
    if (!bytecode_err.first) {
      return runtime->raiseSyntaxError(TwineChar16(bytecode_err.second));
    }
    if (singleFunction && !bytecode_err.first->isSingleFunction()) {
      return runtime->raiseSyntaxError("Invalid function expression");
    }
    bytecode = std::move(bytecode_err.first);
  }

  // TODO: pass a sourceURL derived from a '//# sourceURL' comment.
  llvh::StringRef sourceURL{};
  return runtime->runBytecode(
      std::move(bytecode), RuntimeModuleFlags{}, sourceURL, scope, thisArg);
#endif
}

CallResult<HermesValue> evalInScope(
    Runtime *runtime,
    Handle<StringPrimitive> str,
    Handle<JSObject> scope,
    Handle<> thisArg,
    LocalEvalFlags localEvalFlags,
    bool singleFunction) {
  // Convert the code into UTF8.
  std::string code;
  auto view = StringPrimitive::createStringView(runtime, str);
  if (view.isASCII()) {
    code = std::string(view.begin(), view.end());
  } else {
    SmallU16String<4> allocator;
    convertUTF16ToUTF8WithReplacements(code, view.getUTF16Ref(allocator));
  }

  return evalInScope(
      runtime, code, scope, thisArg, localEvalFlags, singleFunction);
}

CallResult<HermesValue> eval(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope(runtime);

  if (!args.getArg(0).isString()) {
    return args.getArg(0);
  }

  return evalInScope(
      runtime,
      args.dyncastArg<StringPrimitive>(0),
      runtime->getGlobal(),
      runtime->getGlobal(),
      LocalEvalFlags{},
      false);
}

} // namespace vm
} // namespace hermes
