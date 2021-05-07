/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IRGEN_IRGEN_H
#define HERMES_IRGEN_IRGEN_H

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"

#include <vector>

namespace hermes {

using DeclarationFileListTy = std::vector<ESTree::ProgramNode *>;

namespace hbc {

class BytecodeFunction;

struct LazyCompilationData {
  /// The original name of the function, as found in the source.
  Identifier originalName;

  /// The source buffer ID in which we can find the function source.
  uint32_t bufferId;

  /// The type of function, e.g. statement or expression.
  ESTree::NodeKind nodeKind;

  /// Flags describing the source context.
  LocalEvalFlags localEvalFlags;
};
} // namespace hbc

/// Lowers an ESTree program into Hermes IR in \p M.
/// \param declFileList a list of parsed global property definition files.
/// \param scopeChain identifiers in the environment, if compiling for local
/// eval.
void generateIRForProgram(
    ESTree::ProgramNode *programNode,
    Module *M,
    const DeclarationFileListTy &declFileList);

/// Compile the specified AST as eval(). All variables are declared in the
/// parent context.
void generateIRForEval(
    ESTree::ProgramNode *programNode,
    Module *M,
    LocalEvalFlags localEvalFlags);

/// Lowers an ESTree program into Hermes IR in \p M without a top-level
/// function, so that it can be used as a CommonJS module.
/// The same module can occur in any number of segments and all such copies of a
/// module are interchangeable at runtime, but the IR is not shared across them.
/// \param segmentID the ID of the segment containing this module.
/// \param id the ID assigned to the CommonJS module when added to the IR
///           (index when reading filenames for the first time).
/// \param filename the relative filename to the CommonJS module.
void generateIRForCJSModule(
    ESTree::FunctionExpressionNode *node,
    uint32_t segmentID,
    uint32_t id,
    llvh::StringRef filename,
    Module *M,
    Function *topLevelFunction,
    const DeclarationFileListTy &declFileList);

/// Generate IR from the AST of a previously pre-parsed "lazy" function by
/// parsing it again and validating it. On error, a stub function which throws
/// a SyntaxError will be emitted instead.
/// \return the newly generated function IR
Function *generateLazyFunctionIR(
    hbc::BytecodeFunction *bcFunction,
    Module *M,
    llvh::SMRange sourceRange);

} // namespace hermes

#endif
