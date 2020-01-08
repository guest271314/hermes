/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ValidatorHelpers.h"
#include "RecursiveVisitor.h"

#include "hermes/ADT/ScopedHashTable.h"
#include "hermes/Support/SourceErrorManager.h"

namespace hermes {
namespace sem {

using llvm::dyn_cast;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Keywords

Keywords::Keywords(Context &astContext)
    : identArguments(
          astContext.getIdentifier("arguments").getUnderlyingPointer()),
      identEval(astContext.getIdentifier("eval").getUnderlyingPointer()),
      identDelete(astContext.getIdentifier("delete").getUnderlyingPointer()),
      identUseStrict(
          astContext.getIdentifier("use strict").getUnderlyingPointer()),
      identVar(astContext.getIdentifier("var").getUnderlyingPointer()),
      identLet(astContext.getIdentifier("let").getUnderlyingPointer()),
      identConst(astContext.getIdentifier("const").getUnderlyingPointer()),
      identTypeof(astContext.getIdentifier("typeof").getUnderlyingPointer()) {}

namespace {

/// This class checks whether it is safe to promote block-scoped function
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
class ScopedFuncChecker {
 public:
  ScopedFuncChecker(Context &astContext, const Keywords &kw)
      : astContext_(astContext), kw_(kw) {}

  void run(FunctionLikeNode *funcNode);

  /// Handle the default case for all nodes which we ignore, but we still want
  /// to visit their children.
  void visit(Node *node) {
    visitESTreeChildren(*this, node);
  }

  /// Do not descend into nested functions.
  void visit(FunctionLikeNode *) {}

  // All nodes with scopes.

  void visit(SwitchStatementNode *node) {
    visitScope(node, node);
  }
  void visit(BlockStatementNode *node) {
    visitScope(node, node);
  }
  void visit(ForStatementNode *node) {
    visitScope(node, node);
  }
  void visit(ForInStatementNode *node) {
    visitScope(node, node);
  }
  void visit(ForOfStatementNode *node) {
    visitScope(node, node);
  }

 private:
  /// Visit any statement starting a scope.
  void visitScope(Node *node, ScopeDecorationBase *scope);

  /// Process the declarations in a scope. This is the core of the algorithm,
  /// It updates the binding tables, etc.
  void processDeclarations(ScopeDecorationBase *scope);

 private:
  /// The AST context.
  Context &astContext_;

  /// Keywords we compare against in certain cases.
  const Keywords &kw_;

  /// The names of the scoped functions. We will ignore all other identifiers.
  llvm::SmallDenseSet<UniqueString *> funcNames_{};

  /// The scoped function declarations. We remove each from this set once
  /// we encounter it.
  llvm::SmallDenseSet<FunctionDeclarationNode *> funcDecls_{};

  using BindingTableTy = hermes::ScopedHashTable<UniqueString *, bool>;
  using BindingTableScopeTy =
      hermes::ScopedHashTableScope<UniqueString *, bool>;

  /// The currently lexically visible names.
  BindingTableTy bindingTable_{};

  /// The function scope.
  ScopeDecorationBase *functionScope_ = nullptr;
};

void ScopedFuncChecker::run(FunctionLikeNode *funcNode) {
  if (funcNode->scopedFuncDecls.empty())
    return;

  // Note that if scopedFuncDecls is not empty, we must have a block statement
  // body (otherwose the scoped functions have nowhere to come from).
  functionScope_ = cast<BlockStatementNode>(getBody(funcNode));

  // Populate the sets.
  for (auto *node : funcNode->scopedFuncDecls) {
    auto *funcDecl = cast<FunctionDeclarationNode>(node);
    funcNames_.insert(cast<IdentifierNode>(funcDecl->_id)->_name);
    funcDecls_.insert(funcDecl);
  }

  visitESTreeChildren(*this, funcNode);
}

void ScopedFuncChecker::visitScope(
    hermes::ESTree::Node *node,
    ScopeDecorationBase *scope) {
  BindingTableScopeTy bindingScope{bindingTable_};
  processDeclarations(scope);
  visitESTreeChildren(*this, node);
}

void ScopedFuncChecker::processDeclarations(ScopeDecorationBase *scope) {
  llvm::SmallVector<IdentifierNode *, 4> idents{};
  // Whenever we encounter one of the scoped func decls we are trying to
  // promote, we store the address of its list entry here (so we can clear it
  // if we want to).
  llvm::SmallVector<Node **, 4> foundDecls{};

  for (auto &nodeRef : scope->decls) {
    auto *node = nodeRef;
    if (!node)
      continue;

    // Did we encounter one the candidate declarations?
    if (auto *funcDecl = dyn_cast<FunctionDeclarationNode>(node)) {
      if (funcDecls_.count(funcDecl))
        foundDecls.push_back(&nodeRef);
      continue;
    }

    idents.clear();
    // Extract idents, do report errors.
    Decl::Kind declKind =
        helperExtractDeclaredIdents(node, idents, kw_, nullptr);

    // We are only interested in let-like declarations.
    if (!Decl::isKindLetLike(declKind))
      continue;

    // Remember only idents matching the set.
    for (auto *idNode : idents) {
      if (funcNames_.count(idNode->_name))
        bindingTable_.insert(idNode->_name, true);
    }
  }

  // Did we finally encounter one of the scoped function declarations?
  for (Node **funcDeclRef : foundDecls) {
    auto *funcDecl = cast<FunctionDeclarationNode>(*funcDeclRef);
    // Remove it from the set, since we are no longer interested in it.
    funcDecls_.erase(funcDecl);

    // Is there a visible let-like declaration with the same name?
    if (bindingTable_.lookup(cast<IdentifierNode>(funcDecl->_id)->_name)) {
      // It can't be promoted, since it would shadow a "let".
      continue;
    }

    // This block-scoped function declaration can (and should) be promoted.
    // 1. Clear it from the current scope's list.
    *funcDeclRef = nullptr;
    // 2. Add it to the function scope list.
    functionScope_->decls.push_back(astContext_, funcDecl);
  }
}

} // anonymous namespace

void promoteScopedFuncDecls(
    Context &astContext,
    const Keywords &kw,
    FunctionLikeNode *funcNode) {
  // Most of the time there is nothing to promote (we hope).
  if (LLVM_LIKELY(funcNode->scopedFuncDecls.empty()))
    return;
  ScopedFuncChecker{astContext, kw}.run(funcNode);
}

void helperExtractDeclaredIdentsFromID(
    Node *node,
    llvm::SmallVectorImpl<IdentifierNode *> &idents,
    SourceErrorManager *sm) {
  // The identifier is sometimes optional, in which case it is valid.
  if (!node)
    return;

  if (auto *idNode = dyn_cast<IdentifierNode>(node)) {
    idents.push_back(idNode);
    return;
  }

  if (isa<EmptyNode>(node))
    return;

  if (auto *assign = dyn_cast<AssignmentPatternNode>(node))
    return helperExtractDeclaredIdentsFromID(assign->_left, idents, sm);

  if (auto *array = dyn_cast<ArrayPatternNode>(node)) {
    for (auto &elem : array->_elements) {
      helperExtractDeclaredIdentsFromID(&elem, idents, sm);
    }
    return;
  }

  if (auto *restElem = dyn_cast<RestElementNode>(node)) {
    return helperExtractDeclaredIdentsFromID(restElem->_argument, idents, sm);
  }

  if (auto *obj = dyn_cast<ObjectPatternNode>(node)) {
    for (auto &propNode : obj->_properties) {
      if (auto *prop = dyn_cast<PropertyNode>(&propNode)) {
        helperExtractDeclaredIdentsFromID(prop->_value, idents, sm);
      } else {
        auto *rest = cast<RestElementNode>(&propNode);
        helperExtractDeclaredIdentsFromID(rest->_argument, idents, sm);
      }
    }
    return;
  }

  if (sm)
    sm->error(node->getSourceRange(), "invalid destructuring target");
}

Decl::Kind helperExtractDeclaredIdents(
    Node *declarationNode,
    llvm::SmallVectorImpl<IdentifierNode *> &idents,
    const Keywords &kw,
    SourceErrorManager *sm) {
  Decl::Kind declKind;

  if (auto *varDeclaration =
          dyn_cast<VariableDeclarationNode>(declarationNode)) {
    if (varDeclaration->_kind == kw.identLet)
      declKind = Decl::Kind::Let;
    else if (varDeclaration->_kind == kw.identConst)
      declKind = Decl::Kind::Const;
    else {
      assert(varDeclaration->_kind == kw.identVar);
      declKind = Decl::Kind::Var;
    }

    for (auto &declarator : varDeclaration->_declarations) {
      helperExtractDeclaredIdentsFromID(
          cast<VariableDeclaratorNode>(declarator)._id, idents, sm);
    }
  } else if (auto *FD = dyn_cast<FunctionDeclarationNode>(declarationNode)) {
    declKind = Decl::Kind::ScopedFunction;
    helperExtractDeclaredIdentsFromID(FD->_id, idents, sm);
  } else if (auto *CD = dyn_cast<ClassDeclarationNode>(declarationNode)) {
    declKind = Decl::Kind::Class;
    helperExtractDeclaredIdentsFromID(CD->_id, idents, sm);
  } else if (auto *ID = dyn_cast<ImportDeclarationNode>(declarationNode)) {
    declKind = Decl::Kind::Import;
    for (auto &spec : ID->_specifiers) {
      if (auto *IS = dyn_cast<ImportSpecifierNode>(&spec)) {
        helperExtractDeclaredIdentsFromID(IS->_local, idents, sm);
      } else if (auto *IDS = dyn_cast<ImportDefaultSpecifierNode>(&spec)) {
        helperExtractDeclaredIdentsFromID(IDS->_local, idents, sm);
      } else {
        auto *INS = cast<ImportNamespaceSpecifierNode>(&spec);
        helperExtractDeclaredIdentsFromID(INS->_local, idents, sm);
      }
    }
  } else {
    llvm_unreachable("unsupported declaration kind");
  }

  return declKind;
}

} // namespace sem
} // namespace hermes
