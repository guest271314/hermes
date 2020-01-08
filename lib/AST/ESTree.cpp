/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/ESTree.h"

#include "llvm/Support/raw_ostream.h"

using llvm::dyn_cast;
using llvm::isa;

namespace hermes {
namespace ESTree {

Node *getIdentifier(FunctionLikeNode *node) {
  switch (node->getKind()) {
    default:
      assert(
          node->getKind() == NodeKind::Program && "invalid FunctionLikeNode");
      return nullptr;
    case NodeKind::FunctionExpression:
      return cast<FunctionExpressionNode>(node)->_id;
    case NodeKind::ArrowFunctionExpression:
      return cast<ArrowFunctionExpressionNode>(node)->_id;
    case NodeKind::FunctionDeclaration:
      return cast<FunctionDeclarationNode>(node)->_id;
  }
}

NodeList &getParams(FunctionLikeNode *node) {
  switch (node->getKind()) {
    default:
      assert(
          node->getKind() == NodeKind::Program && "invalid FunctionLikeNode");
      return cast<ProgramNode>(node)->dummyParamList;
    case NodeKind::FunctionExpression:
      return cast<FunctionExpressionNode>(node)->_params;
    case NodeKind::ArrowFunctionExpression:
      return cast<ArrowFunctionExpressionNode>(node)->_params;
    case NodeKind::FunctionDeclaration:
      return cast<FunctionDeclarationNode>(node)->_params;
  }
}

Node *getBody(FunctionLikeNode *node) {
  switch (node->getKind()) {
    default:
      assert(
          node->getKind() == NodeKind::Program && "invalid FunctionLikeNode");
      return nullptr;
    case NodeKind::FunctionExpression:
      return cast<FunctionExpressionNode>(node)->_body;
    case NodeKind::ArrowFunctionExpression: {
      return cast<ArrowFunctionExpressionNode>(node)->_body;
      case NodeKind::FunctionDeclaration:
        return cast<FunctionDeclarationNode>(node)->_body;
    }
  }
}

Node *getObject(MemberExpressionLikeNode *node) {
  switch (node->getKind()) {
    case NodeKind::MemberExpression:
      return cast<MemberExpressionNode>(node)->_object;
    case NodeKind::OptionalMemberExpression:
      return cast<OptionalMemberExpressionNode>(node)->_object;
    default:
      break;
  }
  llvm_unreachable("invalid MemberExpressionLikeNode");
}

Node *getProperty(MemberExpressionLikeNode *node) {
  switch (node->getKind()) {
    case NodeKind::MemberExpression:
      return cast<MemberExpressionNode>(node)->_property;
    case NodeKind::OptionalMemberExpression:
      return cast<OptionalMemberExpressionNode>(node)->_property;
    default:
      break;
  }
  llvm_unreachable("invalid MemberExpressionLikeNode");
}

NodeBoolean getComputed(MemberExpressionLikeNode *node) {
  switch (node->getKind()) {
    case NodeKind::MemberExpression:
      return cast<MemberExpressionNode>(node)->_computed;
    case NodeKind::OptionalMemberExpression:
      return cast<OptionalMemberExpressionNode>(node)->_computed;
    default:
      break;
  }
  llvm_unreachable("invalid MemberExpressionLikeNode");
}

Node *getCallee(CallExpressionLikeNode *node) {
  switch (node->getKind()) {
    case NodeKind::CallExpression:
      return cast<CallExpressionNode>(node)->_callee;
    case NodeKind::OptionalCallExpression:
      return cast<OptionalCallExpressionNode>(node)->_callee;
    default:
      break;
  }
  llvm_unreachable("invalid CallExpressionLikeNode");
}

NodeList &getArguments(CallExpressionLikeNode *node) {
  switch (node->getKind()) {
    case NodeKind::CallExpression:
      return cast<CallExpressionNode>(node)->_arguments;
    case NodeKind::OptionalCallExpression:
      return cast<OptionalCallExpressionNode>(node)->_arguments;
    default:
      break;
  }
  llvm_unreachable("invalid CallExpressionLikeNode");
}

bool hasSimpleParams(FunctionLikeNode *node) {
  for (Node &param : getParams(node)) {
    if (isa<PatternNode>(param))
      return false;
  }
  return true;
}

bool isLazyFunction(FunctionLikeNode *node) {
  if (auto *block = dyn_cast<BlockStatementNode>(getBody(node))) {
    return block->isLazyFunctionBody;
  }
  return false;
}

} // namespace ESTree
} // namespace hermes
