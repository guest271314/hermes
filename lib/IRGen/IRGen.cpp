/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IRGen/IRGen.h"

#include "ESTreeIRGen.h"

#include "hermes/AST/ESTreeJSONDumper.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Support/SimpleDiagHandler.h"

namespace hermes {

using namespace hermes::irgen;
using llvm::dbgs;

bool generateIRFromESTree(
    ESTree::NodePtr node,
    Module *M,
    sem::SemContext &semCtx,
    const ScopeChain &scopeChain) {
  // Generate IR into the module M.
  ESTreeIRGen Generator(node, M, semCtx, scopeChain);
  Generator.doIt();

  LLVM_DEBUG(dbgs() << "Finished IRGen.\n");
  return false;
}

void generateIRForCJSModule(
    ESTree::FunctionExpressionNode *node,
    uint32_t id,
    llvm::StringRef filename,
    Module *M,
    Function *topLevelFunction,
    sem::SemContext &semCtx) {
  // Generate IR into the module M.
  ESTreeIRGen generator(node, M, semCtx, {});
  return generator.doCJSModule(
      topLevelFunction, node->getSemInfo(), id, filename);
}

std::pair<Function *, Function *> generateLazyFunctionIR(
    hbc::LazyCompilationData *lazyData,
    Module *M) {
  auto &context = M->getContext();
  SimpleDiagHandlerRAII diagHandler{context.getSourceErrorManager()};

  AllocationScope alloc(context.getAllocator());
  sem::SemContext semCtx{context, DeclarationFileListTy{}};
  hermes::parser::JSParser parser(
      context, lazyData->bufferId, parser::LazyParse);

  // Note: we don't know the parent's strictness, which we need to pass, but
  // we can just use the child's strictness, which is always stricter or equal
  // to the parent's.
  parser.setStrictMode(lazyData->strictMode);

  auto parsed = parser.parseLazyFunction(
      (ESTree::NodeKind)lazyData->nodeKind, lazyData->span.Start);

  if (!diagHandler.haveErrors() &&
      context.getLazyDumpTarget() == OutputFormatKind::DumpAST) {
    llvm::outs() << "**** Lazy dump ****\n\n";
    hermes::dumpESTreeJSON(
        llvm::outs(),
        *parsed,
        true /* pretty */,
        &context.getSourceErrorManager());
  }

  if (!diagHandler.haveErrors()) {
    sem::validateFunctionAST(
        context,
        semCtx,
        *parsed,
        lazyData->strictMode ? ESTree::Strictness::StrictMode
                             : ESTree::Strictness::NonStrictMode);
  }

  if (!diagHandler.haveErrors() &&
      context.getLazyDumpTarget() == OutputFormatKind::DumpTransformedAST) {
    llvm::outs() << "**** Lazy dump ****\n\n";
    hermes::dumpESTreeJSON(
        llvm::outs(),
        *parsed,
        true /* pretty */,
        &context.getSourceErrorManager());
  }

  // In case of error, generate a function that just throws a SyntaxError.
  if (diagHandler.haveErrors()) {
    LLVM_DEBUG(
        llvm::dbgs() << "Lazy AST parsing/validation failed with error: "
                     << diagHandler.getErrorString());

    auto *error = ESTreeIRGen::genSyntaxErrorFunction(
        M,
        lazyData->originalName,
        lazyData->span,
        diagHandler.getErrorString());

    return {error, error};
  }

  ESTreeIRGen generator{parsed.getValue(), M, semCtx, {}};
  return generator.doLazyFunction(lazyData);
}

} // namespace hermes
