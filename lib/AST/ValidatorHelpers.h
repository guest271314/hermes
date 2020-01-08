/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_AST_VALIDATORHELPERS_H
#define HERMES_AST_VALIDATORHELPERS_H

#include "hermes/AST/ESTree.h"
#include "hermes/AST/SemValidate.h"

namespace hermes {
namespace sem {

using namespace hermes::ESTree;

//===----------------------------------------------------------------------===//
// Keywords

class Keywords {
 public:
  /// Identifier for "arguments".
  const UniqueString *const identArguments;
  /// Identifier for "eval".
  const UniqueString *const identEval;
  /// Identifier for "delete".
  const UniqueString *const identDelete;
  /// Identifier for "use strict".
  const UniqueString *const identUseStrict;
  /// Identifier for "var".
  const UniqueString *const identVar;
  /// Identifier for "let".
  const UniqueString *const identLet;
  /// Identifier for "const".
  const UniqueString *const identConst;
  /// Identifier for "typeof".
  const UniqueString *const identTypeof;

  explicit Keywords(Context &astContext);
};

//===----------------------------------------------------------------------===//

/// This function checks whether it is safe to promote block-scoped function
/// declarations to function scope. i.e. whether it is safe to replace one with
/// "var" without creating a conflict.
///
/// A conflict exists if a lex-like declaration is visible in the declaration
/// scope. The checker starts with a list of all block scoped function
/// declarations. Then it visits all scopes recursively, maintaining a scoped
/// table of let-like declarations with matching names. When it encounters a
/// block-scoped function declaration, it checks whether a matching let-like
/// declaration is visible. If not, it is safe to promote.
///
/// When a function declaration is promoted,
///
/// The input is a list of block-scoped function function declarations. The
/// ones that can be promoted are deleted from their own scope and added to the
/// function scope.
void promoteScopedFuncDecls(
    Context &astContext,
    const Keywords &kw,
    FunctionLikeNode *funcNode);

/// Extract the declared identifiers from a declaration AST node's "id" field.
/// Normally that is just a single identifier, but it can be more in case of
/// destructuring.
/// \param sm optional error manager to report errors. If nullptr, no errors
///     are reported.
void helperExtractDeclaredIdentsFromID(
    Node *node,
    llvm::SmallVectorImpl<IdentifierNode *> &idents,
    SourceErrorManager *sm);

/// Extract the list of declared identifiers in a declaration node and return
/// the declaration kind of the node. Function declarations are always returned
/// as \c Decl::Kind::ScopedFunction, so they can be distinguished.
/// \param kw the keywords table
/// \param sm optional error manager to report errors. If nullptr, no errors
///     are reported.
Decl::Kind helperExtractDeclaredIdents(
    Node *declarationNode,
    llvm::SmallVectorImpl<IdentifierNode *> &idents,
    const Keywords &kw,
    SourceErrorManager *sm);

} // namespace sem
} // namespace hermes

#endif // HERMES_AST_VALIDATORHELPERS_H
