/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/SemValidate.h"

#include "hermes/Support/PerfSection.h"

#include "SemanticValidator.h"

using namespace hermes::ESTree;

namespace hermes {
namespace sem {

bool validateAST(
    Context &astContext,
    SemContext &semCtx,
    ProgramNode *root,
    bool global) {
  PerfSection validation("Validating JavaScript function AST");
  // Validate the entire AST.
  SemanticValidator validator{astContext, semCtx};
  return validator.doIt(root);
}

bool validateFunctionAST(
    Context &astContext,
    SemContext &semCtx,
    Node *function,
    ESTree::Strictness strict) {
  PerfSection validation("Validating JavaScript function AST: Deep");
  SemanticValidator validator{astContext, semCtx};
  return validator.doFunction(
      function, strict == ESTree::Strictness::StrictMode);
}

} // namespace sem
} // namespace hermes
