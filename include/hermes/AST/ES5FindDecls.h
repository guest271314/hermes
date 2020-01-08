/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_AST_ES5FINDDECLS_H
#define HERMES_AST_ES5FINDDECLS_H

#include "hermes/AST/ESTree.h"

#include "llvm/ADT/SmallVector.h"

namespace hermes {
namespace ESTree {

/// This visitor structs collects declarations within a single closure without
/// descending into child closures.
struct ES5FindDecls {
  /// The list of collected identifiers (variables and functions).
  llvm::SmallVector<ESTree::VariableDeclaratorNode *, 8> decls{};

  /// A list of functions that need to be hoisted and materialized before we
  /// can generate the rest of the function.
  llvm::SmallVector<ESTree::FunctionDeclarationNode *, 8> closures;

  explicit ES5FindDecls() = default;
  ~ES5FindDecls() = default;

  /// Extract the variable name from the nodes that can define new variables.
  /// The nodes that can define a new variable in the scope are:
  /// VariableDeclarator and FunctionDeclaration>
  void collectDecls(ESTree::Node *V) {
    if (auto VD = llvm::dyn_cast<ESTree::VariableDeclaratorNode>(V)) {
      return decls.push_back(VD);
    }

    if (auto FD = llvm::dyn_cast<ESTree::FunctionDeclarationNode>(V)) {
      return closures.push_back(FD);
    }
  }

  bool shouldVisit(ESTree::Node *V) {
    // Collect declared names, even if we don't descend into children nodes.
    collectDecls(V);

    // Do not descend to child closures because the variables they define are
    // not exposed to the outside function.
    if (llvm::isa<ESTree::FunctionDeclarationNode>(V) ||
        llvm::isa<ESTree::FunctionExpressionNode>(V) ||
        llvm::isa<ESTree::ArrowFunctionExpressionNode>(V))
      return false;
    return true;
  }

  void enter(ESTree::Node *V) {}
  void leave(ESTree::Node *V) {}
};
} // namespace ESTree
} // namespace hermes

#endif // HERMES_AST_ES5FINDDECLS_H
