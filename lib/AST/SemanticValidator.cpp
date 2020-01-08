/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SemanticValidator.h"

#include "hermes/AST/ESTreeJSONDumper.h"
#include "hermes/Support/RegExpSerialization.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/SaveAndRestore.h"

#define DEBUG_TYPE "semval"

using llvm::cast;
using llvm::cast_or_null;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::SaveAndRestore;

using namespace hermes::ESTree;

namespace hermes {
namespace sem {

//===----------------------------------------------------------------------===//
// SemanticValidator

SemanticValidator::SemanticValidator(
    Context &astContext,
    sem::SemContext &semCtx,
    sem::LexicalScope *lexicalScope)
    : astContext_(astContext),
      sm_(astContext.getSourceErrorManager()),
      bufferMessages_{&sm_},
      semData_(semCtx.getData()),
      initialErrorCount_(sm_.getErrorCount()),
      kw_(astContext) {
  semData_.forEachScopeUpTo(lexicalScope, [this](LexicalScope *sc) {
    scopes_.emplace_back(this, ScopeRAII::DontPush{});
    // Bind all properties.
    for (auto *decl : sc->decls) {
      bindingTable_.insert(
          decl->name.getUnderlyingPointer(), Binding{decl, nullptr});
    }
  });

  globalScope_ = &scopes_.at(0).getBindingScope();

  declareGlobals();

  semData_.setCurScope(lexicalScope);
}

SemanticValidator::~SemanticValidator() {
  // Destroy the scopes in deterministic order.
  while (!scopes_.empty())
    scopes_.pop_back();
}

bool SemanticValidator::doIt(ProgramNode *rootNode, bool global) {
  visitProgram(rootNode, global);
  return sm_.getErrorCount() == initialErrorCount_;
}

bool SemanticValidator::doFunction(Node *function, bool strict) {
  // Create a wrapper context since a function always assumes there is an
  // existing context.
  FunctionContext wrapperContext(
      this, strict, nullptr, semData_.getCurFunction());

  visitESTreeNode(*this, function);
  return sm_.getErrorCount() == initialErrorCount_;
}

bool SemanticValidator::doLazyFunction(Node *function, bool strict) {
  // Create a wrapper context since a function always assumes there is an
  // existing context.
  FunctionContext wrapperContext(
      this, strict, nullptr, semData_.getCurFunction());

  if (auto *FE = dyn_cast<FunctionExpressionNode>(function)) {
    sm_.error(function->getStartLoc(), "Unsupported lazy function");
  } else if (auto *funcDecl = dyn_cast<FunctionDeclarationNode>(function)) {
    visitFunction(funcDecl, funcDecl->_params, funcDecl->_body);
  } else if (auto *P = dyn_cast<PropertyNode>(function)) {
    sm_.error(function->getStartLoc(), "Unsupported lazy function");
  } else {
    sm_.error(function->getStartLoc(), "Unsupported lazy function");
  }

  return sm_.getErrorCount() == initialErrorCount_;
}

void SemanticValidator::visitProgram(ProgramNode *node, bool global) {
  assert(
      semData_.getCurFunction() == semData_.getGlobalFunction() &&
      "ProgramNode must be in the global function");
  assert(
      semData_.getCurScope() == semData_.getGlobalScope() &&
      "ProgramNode must be in the global scope");

#ifndef NDEBUG
  strictnessIsPreset_ = node->strictness != Strictness::NotSet;
#endif
  FunctionContext newFuncCtx{this,
                             astContext_.isStrictMode(),
                             node,
                             global ? semData_.getGlobalFunction() : nullptr};

  scanDirectivePrologue(node->_body);
  updateNodeStrictness(node);

  llvm::Optional<ScopeRAII> nameScope;

  if (!global) {
    nameScope.emplace(this);
  }

  processDeclarationsInScope(node);

  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(MetaPropertyNode *metaProp) {
  auto *meta = cast<IdentifierNode>(metaProp->_meta);
  auto *property = cast<IdentifierNode>(metaProp->_property);

  if (meta->_name->str() == "new" && property->_name->str() == "target") {
    if (curFunction()->isGlobalScope()) {
      // ES9.0 15.1.1:
      // It is a Syntax Error if StatementList Contains NewTarget unless the
      // source code containing NewTarget is eval code that is being processed
      // by a direct eval.
      // Hermes does not support local eval, so we assume that this is not
      // inside a local eval call.
      sm_.error(metaProp->getSourceRange(), "'new.target' not in a function");
    }
    return;
  }

  sm_.error(
      metaProp->getSourceRange(),
      "invalid meta property " + meta->_name->str() + "." +
          property->_name->str());
}

void SemanticValidator::visit(IdentifierNode *identifier, Node *parent) {
  // Filter out cases where this is not a variable access.
  if (auto *property = dyn_cast<PropertyNode>(parent)) {
    // { identifier: ... }
    if (!property->_computed && property->_key == identifier)
      return;
  } else if (auto *memberExpr = dyn_cast<MemberExpressionNode>(parent)) {
    // expr.identifier
    if (!memberExpr->_computed && memberExpr->_property == identifier)
      return;
  } else if (isa<MetaPropertyNode>(parent)) {
    // new.target for example.
    return;
  } else if (auto *unary = dyn_cast<UnaryExpressionNode>(parent)) {
    // typeof x.
    if (unary->_operator == kw_.identTypeof)
      return resolveIdentifier(identifier, true);
  } else if (
      isa<BreakStatementNode>(parent) || isa<ContinueStatementNode>(parent)) {
    // break label;
    return;
  } else if (isa<LabeledStatementNode>(parent)) {
    // label:
    return;
  }

  resolveIdentifier(identifier, false);
}

/// Process a function declaration by creating a new FunctionContext.
void SemanticValidator::visit(FunctionDeclarationNode *funcDecl) {
  // Collect hoisted function.
  auto *curScope = semData_.getCurScope();
  curScope->hoistedFunctions.push_back(funcDecl);

  // If a function is hoisted in its own scope, mark it, to make IRGen's
  // life easier.
  if (auto *decl = cast<IdentifierNode>(funcDecl->_id)->decl)
    if (decl->scope == curScope)
      decl->functionInScope = true;

  visitFunction(funcDecl, funcDecl->_params, funcDecl->_body);
}

/// Process a function expression by creating a new FunctionContext.
void SemanticValidator::visit(FunctionExpressionNode *funcExpr) {
  // A lookup scope for the function expression name.
  ScopeRAII nameScope{this};

  llvm::SmallVector<IdentifierNode *, 1> idents{};
  extractDeclaredIdentsFromID(funcExpr->_id, idents);

  // This shouldn't really be a loop, because we only expect one identifier.
  // But it is more natural to process the vector as a vector.
  assert(idents.size() <= 1 && "Function can't have more than one name");
  for (auto *idNode : idents) {
    if (!validateDeclarationName(Decl::Kind::FunctionExprName, idNode))
      continue;

    auto *decl = semData_.newDecl(idNode->_name, Decl::Kind::FunctionExprName);
    idNode->decl = decl;
    bindingTable_.insert(idNode->_name, Binding{decl, idNode});
  }

  visitFunction(funcExpr, funcExpr->_params, funcExpr->_body);
}

void SemanticValidator::visit(ArrowFunctionExpressionNode *arrowFunc) {
  // Convert expression functions to a full-body to simplify IRGen.
  if (arrowFunc->_expression) {
    auto *retStmt = new (astContext_) ReturnStatementNode(arrowFunc->_body);
    retStmt->copyLocationFrom(arrowFunc->_body);

    ESTree::NodeList stmtList;
    stmtList.push_back(*retStmt);

    auto *blockStmt = new (astContext_) BlockStatementNode(std::move(stmtList));
    blockStmt->copyLocationFrom(arrowFunc->_body);

    arrowFunc->_body = blockStmt;
    arrowFunc->_expression = false;
  }

  visitFunction(arrowFunc, arrowFunc->_params, arrowFunc->_body);

  curFunction()->semInfo->containsArrowFunctions = true;
  curFunction()->semInfo->containsArrowFunctionsUsingArguments |=
      arrowFunc->getSemInfo()->containsArrowFunctionsUsingArguments |
      arrowFunc->getSemInfo()->usesArguments;
}

void SemanticValidator::visit(BlockStatementNode *blockStmt, Node *parent) {
  // Some nodes with attached BlockStatement have already dealt with the scope.
  if ((isa<FunctionLikeNode>(parent) && !isa<ProgramNode>(parent)) ||
      isa<CatchClauseNode>(parent)) {
    return visitESTreeChildren(*this, blockStmt);
  }

  // Create a new lexical scope.
  ScopeRAII nameScope{this};

  blockStmt->setLexicalScope(semData_.getCurScope());

  processDeclarationsInScope(blockStmt);

  visitESTreeChildren(*this, blockStmt);
}

/// Ensure that the left side of for-in is an l-value.
void SemanticValidator::visit(ForInStatementNode *forIn) {
  visitForInOf(forIn, forIn->_left, forIn->_right, forIn->_body, forIn);
}
void SemanticValidator::visit(ForOfStatementNode *forOf) {
  visitForInOf(forOf, forOf->_left, forOf->_right, forOf->_body, forOf);
}

void SemanticValidator::visitForInOf(
    LoopStatementNode *loopNode,
    Node *left,
    Node *right,
    Node *body,
    ScopeDecorationBase *scopeDecoration) {
  loopNode->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(
      curFunction()->activeLoop, loopNode);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loopNode);

  ScopeRAII nameScope{this};
  processDeclarationsInScope(scopeDecoration);
  scopeDecoration->setLexicalScope(semData_.getCurScope());

  visitESTreeNode(*this, left, loopNode);

  if (auto *VD = dyn_cast<VariableDeclarationNode>(left)) {
    assert(
        VD->_declarations.size() == 1 &&
        "for-in/for-of must have a single binding");

    auto *declarator =
        cast<ESTree::VariableDeclaratorNode>(&VD->_declarations.front());

    if (declarator->_init) {
      if (isa<ESTree::PatternNode>(declarator->_id)) {
        sm_.error(
            declarator->_init->getSourceRange(),
            "destructuring declaration cannot be initialized in for-in/for-of loop");
      } else if (!(isa<ForInStatementNode>(loopNode) &&
                   !curFunction()->strictMode && VD->_kind == kw_.identVar)) {
        sm_.error(
            declarator->_init->getSourceRange(),
            "for-in/for-of variable declaration may not be initialized");
      }
    }
  } else {
    validateAssignmentTarget(left);
  }
  visitESTreeNode(*this, right, loopNode);
  visitESTreeNode(*this, body, loopNode);
}

/// Ensure that the left side of assignments is an l-value.
void SemanticValidator::visit(AssignmentExpressionNode *assignment) {
  // Visit the left child first to resolve identifiers.
  visitESTreeNode(*this, assignment->_left, assignment);
  validateAssignmentTarget(assignment->_left);
  visitESTreeNode(*this, assignment->_right, assignment);
}

/// Ensure that the operand of ++/-- is an l-value.
void SemanticValidator::visit(UpdateExpressionNode *update) {
  // Visit the children first to resolve identifiers.
  visitESTreeChildren(*this, update);

  // Check if the left-hand side is valid.
  if (!matchLValue(update->_argument)) {
    sm_.error(
        update->_argument->getSourceRange(),
        "invalid operand in update operation");
    return;
  }
  validateLValue(update->_argument);
}

/// Declare named labels, checking for duplicates, etc.
void SemanticValidator::visit(LabeledStatementNode *labelStmt) {
  auto id = cast<IdentifierNode>(labelStmt->_label);

  labelStmt->setLabelIndex(curFunction()->allocateLabel());

  // Determine the target statement. We need to check if it directly encloses
  // a loop or another label enclosing a loop.
  StatementNode *targetStmt = labelStmt;
  {
    LabeledStatementNode *curStmt = labelStmt;
    do {
      if (auto *LS = dyn_cast<LoopStatementNode>(curStmt->_body)) {
        targetStmt = LS;
        break;
      }
    } while ((curStmt = dyn_cast<LabeledStatementNode>(curStmt->_body)));
  }

  // Define the new label, checking for a previous definition.
  auto insertRes =
      curFunction()->labelMap.insert({id->_name, {id, targetStmt}});
  if (!insertRes.second) {
    sm_.error(
        id->getSourceRange(),
        llvm::Twine("label '") + id->_name->str() + "' is already defined");
    sm_.note(
        insertRes.first->second.declarationNode->getSourceRange(),
        "previous definition");
  }
  // Auto-erase the label on exit, if we inserted it.
  const auto &deleter = llvm::make_scope_exit([=]() {
    if (insertRes.second)
      curFunction()->labelMap.erase(id->_name);
  });
  (void)deleter;

  visitESTreeChildren(*this, labelStmt);
}

/// Check RegExp syntax.
void SemanticValidator::visit(RegExpLiteralNode *regexp) {
  llvm::StringRef regexpError;
  if (!CompiledRegExp::tryCompile(
          regexp->_pattern->str(), regexp->_flags->str(), &regexpError)) {
    sm_.error(
        regexp->getSourceRange(),
        "Invalid regular expression: " + Twine(regexpError));
  }
  visitESTreeChildren(*this, regexp);
}

void SemanticValidator::visit(TryStatementNode *tryStatement) {
  // A try statement with both catch and finally handlers is technically
  // two nested try statements. Transform:
  //
  //    try {
  //      tryBody;
  //    } catch {
  //      catchBody;
  //    } finally {
  //      finallyBody;
  //    }
  //
  // into
  //
  //    try {
  //      try {
  //        tryBody;
  //      } catch {
  //        catchBody;
  //      }
  //    } finally {
  //      finallyBody;
  //    }
  if (tryStatement->_handler && tryStatement->_finalizer) {
    auto *nestedTry = new (astContext_)
        TryStatementNode(tryStatement->_block, tryStatement->_handler, nullptr);
    nestedTry->copyLocationFrom(tryStatement);
    nestedTry->setEndLoc(nestedTry->_handler->getEndLoc());

    ESTree::NodeList stmtList;
    stmtList.push_back(*nestedTry);
    tryStatement->_block =
        new (astContext_) BlockStatementNode(std::move(stmtList));
    tryStatement->_block->copyLocationFrom(nestedTry);
    tryStatement->_handler = nullptr;
  }

  visitESTreeNode(*this, tryStatement->_block, tryStatement);
  visitESTreeNode(*this, tryStatement->_handler, tryStatement);
  visitESTreeNode(*this, tryStatement->_finalizer, tryStatement);
}

void SemanticValidator::visit(CatchClauseNode *catchClause) {
  // A lookup scope for the catch expression name.
  ScopeRAII nameScope{this};

  // For compatibility with ES5, we need to treat a single catch variable
  // specially, see: B.3.5 VariableStatements in Catch Blocks
  // https://www.ecma-international.org/ecma-262/10.0/index.html#sec-variablestatements-in-catch-blocks
  if (auto *idNode = dyn_cast<IdentifierNode>(catchClause->_param)) {
    validateAndDeclareIdentifier(Decl::Kind::ES5Catch, idNode, idNode);
  } else {
    llvm::SmallVector<IdentifierNode *, 1> idents{};
    extractDeclaredIdentsFromID(catchClause->_param, idents);

    for (auto *idNode : idents)
      validateAndDeclareIdentifier(Decl::Kind::Let, idNode, idNode);
  }

  auto *blockStmt = cast<BlockStatementNode>(catchClause->_body);
  processDeclarationsInScope(blockStmt);

  visitESTreeChildren(*this, catchClause);
}

void SemanticValidator::visit(DoWhileStatementNode *loop) {
  loop->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(curFunction()->activeLoop, loop);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loop);

  visitESTreeChildren(*this, loop);
}
void SemanticValidator::visit(ForStatementNode *loop) {
  loop->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(curFunction()->activeLoop, loop);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loop);

  ScopeRAII nameScope{this};
  processDeclarationsInScope(loop);
  loop->setLexicalScope(semData_.getCurScope());

  visitESTreeChildren(*this, loop);
}
void SemanticValidator::visit(WhileStatementNode *loop) {
  loop->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<LoopStatementNode *> saveLoop(curFunction()->activeLoop, loop);
  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, loop);

  visitESTreeChildren(*this, loop);
}
void SemanticValidator::visit(SwitchStatementNode *switchStmt) {
  // Visit the discriminant before creating a new scope.
  visitESTreeNode(*this, switchStmt->_discriminant, switchStmt);

  switchStmt->setLabelIndex(curFunction()->allocateLabel());

  SaveAndRestore<StatementNode *> saveSwitch(
      curFunction()->activeSwitchOrLoop, switchStmt);

  ScopeRAII nameScope{this};
  switchStmt->setLexicalScope(semData_.getCurScope());
  processDeclarationsInScope(switchStmt);

  visitESTreeNode(*this, switchStmt->_cases, switchStmt);
}

void SemanticValidator::visit(BreakStatementNode *breakStmt) {
  if (auto id = cast_or_null<IdentifierNode>(breakStmt->_label)) {
    // A labeled break.
    // Find the label in the label map.
    auto labelIt = curFunction()->labelMap.find(id->_name);
    if (labelIt != curFunction()->labelMap.end()) {
      auto labelIndex = getLabelDecorationBase(labelIt->second.targetStatement)
                            ->getLabelIndex();
      breakStmt->setLabelIndex(labelIndex);
    } else {
      sm_.error(
          id->getSourceRange(),
          Twine("label '") + id->_name->str() + "' is not defined");
    }
  } else {
    // Anonymous break.
    if (curFunction()->activeSwitchOrLoop) {
      auto labelIndex =
          getLabelDecorationBase(curFunction()->activeSwitchOrLoop)
              ->getLabelIndex();
      breakStmt->setLabelIndex(labelIndex);
    } else {
      sm_.error(
          breakStmt->getSourceRange(), "'break' not within a loop or a switch");
    }
  }

  visitESTreeChildren(*this, breakStmt);
}

void SemanticValidator::visit(ContinueStatementNode *continueStmt) {
  if (auto id = cast_or_null<IdentifierNode>(continueStmt->_label)) {
    // A labeled continue.
    // Find the label in the label map.
    auto labelIt = curFunction()->labelMap.find(id->_name);
    if (labelIt != curFunction()->labelMap.end()) {
      if (isa<LoopStatementNode>(labelIt->second.targetStatement)) {
        auto labelIndex =
            getLabelDecorationBase(labelIt->second.targetStatement)
                ->getLabelIndex();
        continueStmt->setLabelIndex(labelIndex);
      } else {
        sm_.error(
            id->getSourceRange(),
            llvm::Twine("continue label '") + id->_name->str() +
                "' is not a loop label");
        sm_.note(
            labelIt->second.declarationNode->getSourceRange(),
            "label defined here");
      }
    } else {
      sm_.error(
          id->getSourceRange(),
          Twine("label '") + id->_name->str() + "' is not defined");
    }
  } else {
    // Anonymous continue.
    if (curFunction()->activeLoop) {
      auto labelIndex = curFunction()->activeLoop->getLabelIndex();
      continueStmt->setLabelIndex(labelIndex);
    } else {
      sm_.error(continueStmt->getSourceRange(), "'continue' not within a loop");
    }
  }
  visitESTreeChildren(*this, continueStmt);
}

void SemanticValidator::visit(ReturnStatementNode *returnStmt) {
  if (curFunction()->isGlobalScope())
    sm_.error(returnStmt->getSourceRange(), "'return' not in a function");
  visitESTreeChildren(*this, returnStmt);
}

void SemanticValidator::visit(YieldExpressionNode *yieldExpr) {
  if (curFunction()->isGlobalScope())
    sm_.error(
        yieldExpr->getSourceRange(), "'yield' not in a generator function");

  if (isFormalParams_) {
    // For generators functions (the only time YieldExpression is parsed):
    // It is a Syntax Error if UniqueFormalParameters Contains YieldExpression
    // is true.
    sm_.error(
        yieldExpr->getSourceRange(),
        "'yield' not allowed in a formal parameter");
  }

  visitESTreeChildren(*this, yieldExpr);
}

void SemanticValidator::visit(CallExpressionNode *callExpr) {
  // Check for a direct call to eval().
  if (auto *identifier = dyn_cast<ESTree::IdentifierNode>(callExpr->_callee)) {
    if (identifier->_name == kw_.identEval) {
      Binding name = bindingTable_.lookup(identifier->_name);

      if (!name.isValid() ||
          (name.decl->scope == semData_.getGlobalScope() &&
           Decl::isKindVarLike(name.decl->kind))) {
        identifier->decl = semData_.getEvalDecl();
        if (!astContext_.getEnableEval())
          sm_.error(identifier->getSourceRange(), "'eval' is disabled");
      }
    }
  }
  visitESTreeChildren(*this, callExpr);
}

void SemanticValidator::visit(UnaryExpressionNode *unaryExpr) {
  // Check for unqualified delete in strict mode.
  if (unaryExpr->_operator == kw_.identDelete) {
    if (curFunction()->strictMode &&
        isa<IdentifierNode>(unaryExpr->_argument)) {
      sm_.error(
          unaryExpr->getSourceRange(),
          "'delete' of a variable is not allowed in strict mode");
    }
  }
  visitESTreeChildren(*this, unaryExpr);
}

void SemanticValidator::visit(ArrayPatternNode *AP) {
  visitESTreeChildren(*this, AP);
}

void SemanticValidator::visit(SpreadElementNode *S, Node *parent) {
  if (!isa<ESTree::ObjectExpressionNode>(parent) &&
      !isa<ESTree::ArrayExpressionNode>(parent) &&
      !isa<ESTree::CallExpressionNode>(parent) &&
      !isa<ESTree::NewExpressionNode>(parent))
    sm_.error(S->getSourceRange(), "spread operator is not supported");
  visitESTreeChildren(*this, S);
}

void SemanticValidator::visit(ClassExpressionNode *node) {
  SaveAndRestore<bool> oldStrictMode{curFunction()->strictMode, true};
  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(ClassDeclarationNode *node) {
  SaveAndRestore<bool> oldStrictMode{curFunction()->strictMode, true};
  visitESTreeChildren(*this, node);
}

void SemanticValidator::visit(VariableDeclarationNode *varDecl) {
  llvm::SmallVector<IdentifierNode *, 4> idents{};
  Decl::Kind declKind = extractDeclaredIdents(varDecl, idents);

  if (Decl::isKindVarLike(declKind)) {
    for (auto *idNode : idents) {
      validateVarDeclaration(idNode);
    }
  }

  visitESTreeChildren(*this, varDecl);
}

void SemanticValidator::visit(ImportDeclarationNode *importDecl) {
  // Like variable declarations, imported names must be hoisted.
  if (!astContext_.getUseCJSModules()) {
    sm_.error(
        importDecl->getSourceRange(),
        "'import' statement requires module mode");
  }

  curFunction()->semInfo->imports.push_back(importDecl);
  visitESTreeChildren(*this, importDecl);
}

void SemanticValidator::visit(ExportNamedDeclarationNode *exportDecl) {
  if (!astContext_.getUseCJSModules()) {
    sm_.error(
        exportDecl->getSourceRange(),
        "'export' statement requires module mode");
  }

  visitESTreeChildren(*this, exportDecl);
}

void SemanticValidator::visit(ExportDefaultDeclarationNode *exportDecl) {
  if (!astContext_.getUseCJSModules()) {
    sm_.error(
        exportDecl->getSourceRange(),
        "'export' statement requires module mode");
  }

  if (auto *funcDecl =
          dyn_cast<ESTree::FunctionDeclarationNode>(exportDecl->_declaration)) {
    if (!funcDecl->_id) {
      // If the default function declaration has no name, then change it to a
      // FunctionExpression node for cleaner IRGen.
      exportDecl->_declaration =
          new (astContext_) ESTree::FunctionExpressionNode(
              funcDecl->_id,
              std::move(funcDecl->_params),
              funcDecl->_body,
              funcDecl->_generator);
      exportDecl->_declaration->copyLocationFrom(funcDecl);
    }
  }

  visitESTreeChildren(*this, exportDecl);
}

void SemanticValidator::visit(ExportAllDeclarationNode *exportDecl) {
  if (!astContext_.getUseCJSModules()) {
    sm_.error(
        exportDecl->getSourceRange(),
        "'export' statement requires CommonJS module mode");
  }
  visitESTreeChildren(*this, exportDecl);
}

void SemanticValidator::visit(CoverEmptyArgsNode *CEA) {
  sm_.error(CEA->getSourceRange(), "invalid empty parentheses '( )'");
}

void SemanticValidator::visit(CoverTrailingCommaNode *CTC) {
  sm_.error(CTC->getSourceRange(), "expression expected after ','");
}

void SemanticValidator::visit(CoverInitializerNode *CI) {
  sm_.error(CI->getStartLoc(), "':' expected in property initialization");
}

void SemanticValidator::visit(CoverRestElementNode *R) {
  sm_.error(R->getSourceRange(), "'...' not allowed in this context");
}

void SemanticValidator::declareGlobals() {
  for (auto &decl : semData_.getGlobals()) {
    if (!bindingTable_.find(decl.name.getUnderlyingPointer())) {
      bindingTable_.insertIntoScope(
          globalScope_,
          decl.name.getUnderlyingPointer(),
          Binding{&decl, nullptr});
    }
  }
}

void SemanticValidator::processDeclarationsInScope(
    ScopeDecorationBase *astScope) {
  LLVM_DEBUG(llvm::dbgs() << "Processing declarations in scope\n");

  for (auto *node : astScope->decls) {
    // Skip erased nodes.
    if (!node)
      continue;
    LLVM_DEBUG(llvm::dbgs() << "\nProcessing declaration:\n";
               dumpESTreeJSON(llvm::dbgs(), node, true));

    llvm::SmallVector<IdentifierNode *, 4> idents{};
    Decl::Kind declKind = extractDeclaredIdents(node, idents);

    for (auto *idNode : idents) {
      validateAndDeclareIdentifier(declKind, idNode, node);
    }
  }
}

Decl::Kind SemanticValidator::extractDeclaredIdents(
    Node *declarationNode,
    llvm::SmallVectorImpl<IdentifierNode *> &idents) {
  Decl::Kind declKind =
      helperExtractDeclaredIdents(declarationNode, idents, kw_, &sm_);

  LLVM_DEBUG({
    llvm::dbgs() << "idents:";
    for (auto id : idents)
      llvm::dbgs() << " " << Identifier::getFromPointer(id->_name);
    llvm::dbgs() << "\n";
  });

  // Adjust declaration kind depending on the scope it occurs in.
  if (declKind == Decl::Kind::Var) {
    if (semData_.getCurScope() == semData_.getGlobalScope()) {
      declKind = Decl::Kind::GlobalProperty;
    }
  } else if (declKind == Decl::Kind::ScopedFunction) {
    if (semData_.getCurScope() == semData_.getGlobalScope()) {
      declKind = Decl::Kind::GlobalProperty;
    } else if (
        semData_.getCurScope() ==
        semData_.getCurFunction()->getFunctionScope()) {
      declKind = Decl::Kind::Var;
    }
  }

  return declKind;
}

void SemanticValidator::extractDeclaredIdentsFromID(
    Node *node,
    llvm::SmallVectorImpl<IdentifierNode *> &idents) {
  helperExtractDeclaredIdentsFromID(node, idents, &sm_);
}

void SemanticValidator::validateAndDeclareIdentifier(
    hermes::sem::Decl::Kind declKind,
    IdentifierNode *idNode,
    Node *declNode) {
  if (!validateDeclarationName(declKind, idNode))
    return;

  auto prevName = bindingTable_.lookup(idNode->_name);

  // Ignore declarations in enclosing functions.
  if (prevName.isValid() &&
      prevName.decl->scope->parentFunction != semData_.getCurFunction()) {
    prevName.invalidate();
  }

  Decl *decl = nullptr;

  // Handle re-declarations, ignoring ambient properties.
  if (prevName.isValid() &&
      prevName.decl->kind != Decl::Kind::UndeclaredGlobalProperty) {
    // Check whether the redeclaration is invalid.
    // Note that since "var" declarations have been hoisted to the function
    // scope, we cannot catch cases where "var" follows something declared in a
    // surrounding lexical scope.
    //
    // ES5Catch, var
    //          -> valid, special case ES10 B.3.5, but we can't catch it here.
    // var|scopedFunction, var|scopedFunction
    //          -> always valid
    // let, var
    //          -> always invalid
    // let, scopedFunction
    //          -> invalid if same scope
    // var|scopedFunction|let, let
    //          -> invalid if the same scope

    auto const prevKind = prevName.decl->kind;
    bool const sameScope = prevName.decl->scope == semData_.getCurScope();

    if ((Decl::isKindLetLike(prevKind) && Decl::isKindVarLike(declKind)) ||
        (Decl::isKindLetLike(prevKind) &&
         declKind == Decl::Kind::ScopedFunction && sameScope) ||
        (Decl::isKindLetLike(declKind) && sameScope)) {
      sm_.error(
          idNode->getSourceRange(),
          llvm::Twine("Identifier '") + idNode->_name->str() +
              "' is already declared");
      if (prevName.node)
        sm_.note(prevName.node->getSourceRange(), "previous declaration");
      return;
    }

    // When to create a new declaration?
    //
    // Var, Var -> use prev
    if (Decl::isKindVarLike(prevKind) && Decl::isKindVarLike(declKind)) {
      decl = prevName.decl;
    }
    // Var, ScopedFunc -> if non-strict or same scope, then use prev,
    //                    else declare new
    else if (
        Decl::isKindVarLike(prevKind) &&
        Decl::isKindVarLikeOrScopedFunction(declKind)) {
      if (sameScope || !curFunction()->strictMode)
        decl = prevName.decl;
      else
        decl = nullptr;
    }
    // ScopedFunc, ScopedFunc same scope -> use prev
    // ScopedFunc, ScopedFunc new scope -> declare new
    else if (
        prevKind == Decl::Kind::ScopedFunction &&
        declKind == Decl::Kind::ScopedFunction) {
      if (sameScope) {
        decl = prevName.decl;
      } else {
        decl = nullptr;
      }
    }
    // ScopedFunc, Var -> convert to var
    else if (
        prevKind == Decl::Kind::ScopedFunction &&
        Decl::isKindVarLike(declKind)) {
      assert(
          sameScope &&
          "we can only encounter Var after ScopedFunction in the same scope");
      // Since they are in the same scope, we can simply convert the existing
      // ScopedFunction to Var.
      decl = prevName.decl;
      decl->kind = Decl::Kind::Var;
    } else {
      decl = nullptr;
    }
  }

  if (!decl) {
    if (Decl::isKindGlobal(declKind))
      decl = semData_.newGlobal(idNode->_name, declKind);
    else
      decl = semData_.newDecl(idNode->_name, declKind);
    bindingTable_.insert(idNode->_name, Binding{decl, idNode});
  }

  idNode->decl = decl;
}

void SemanticValidator::validateVarDeclaration(IdentifierNode *idNode) {
  // If this identifier failed validation, it won't have an associated decl.
  if (!idNode->decl)
    return;
  assert(
      Decl::isKindVarLike(idNode->decl->kind) &&
      "we should only validate var declarations here");

  auto prevName = bindingTable_.lookup(idNode->_name);

  assert(
      prevName.isValid() &&
      prevName.decl->scope->parentFunction == semData_.getCurFunction() &&
      "Identifier should have been declared in the current function");

  // Check the remaining cases where "var" follows something declared in a
  // surrounding lexical scope.
  //
  // ES5Catch, var
  //          -> valid, special case ES10 B.3.5, but we can't catch it here.
  // let, var
  //          -> always invalid

  // There is nothing to validate if the same binding is currently visible.
  if (prevName.decl == idNode->decl)
    return;

  auto const prevKind = prevName.decl->kind;

  if (Decl::isKindLetLike(prevKind) && prevKind != Decl::Kind::ES5Catch) {
    sm_.error(
        idNode->getSourceRange(),
        llvm::Twine("Identifier '") + idNode->_name->str() +
            "' is already declared");
    if (prevName.node)
      sm_.note(prevName.node->getSourceRange(), "previous declaration");
    return;
  }

  // "catch(e)" followed by "var e", the second "e" refers to the catch one.
  if (prevKind == Decl::Kind::ES5Catch)
    idNode->decl = prevName.decl;
}

bool SemanticValidator::validateDeclarationName(
    hermes::sem::Decl::Kind declKind,
    const IdentifierNode *idNode) const {
  if (curFunction()->strictMode) {
    // - 'arguments' cannot be redeclared in strict mode.
    // - 'eval' cannot be redeclared in strict mode. If it is disabled we
    // we don't report an error because it will be reported separately.
    if (idNode->_name == kw_.identArguments ||
        (idNode->_name == kw_.identEval && astContext_.getEnableEval())) {
      sm_.error(
          idNode->getSourceRange(),
          "cannot declare '" + cast<IdentifierNode>(idNode)->_name->str() +
              "' in strict mode");
      return false;
    }

    // Parameter cannot be named "let".
    if (declKind == Decl::Kind::Parameter && idNode->_name == kw_.identLet) {
      sm_.error(
          idNode->getSourceRange(),
          "invalid parameter name 'let' in strict mode");
      return false;
    }
  }

  if ((declKind == Decl::Kind::Let || declKind == Decl::Kind::Const) &&
      idNode->_name == kw_.identLet) {
    // ES9.0 13.3.1.1
    // LexicalDeclaration : LetOrConst BindingList
    // It is a Syntax Error if the BoundNames of BindingList
    // contains "let".
    sm_.error(
        idNode->getSourceRange(),
        "'let' is disallowed as a lexically bound name");
    return false;
  }

  return true;
}

void SemanticValidator::visitFunction(
    FunctionLikeNode *node,
    NodeList &params,
    Node *body) {
  if (isLazyFunction(node))
    return;

  FunctionContext newFuncCtx{
      this, haveActiveContext() && curFunction()->strictMode, node, nullptr};

  // It is a Syntax Error if UniqueFormalParameters Contains YieldExpression
  // is true.
  // NOTE: isFormalParams_ is reset to false on encountering a new function,
  // because the semantics for "x Contains y" always return `false` when "x"
  // is a function definition.
  llvm::SaveAndRestore<bool> oldIsFormalParamsFn{isFormalParams_, false};

  ScopeRAII nameScope{this};

  // Points to the optional "use strict" directive in the body.
  Node *useStrictNode = nullptr;

  // The optional block statement body.
  auto *blockStmt = dyn_cast<BlockStatementNode>(body);

  // Note that body might me empty (for lazy functions) or an expression (for
  // arrow functions).
  if (blockStmt) {
    useStrictNode =
        scanDirectivePrologue(cast<ESTree::BlockStatementNode>(body)->_body);
    updateNodeStrictness(node);
  }

  /// Validate the function name. Note that it doesn't really matter what
  /// kind we pass here, as long as it is not parameter/let/const.
  if (auto *name = dyn_cast_or_null<IdentifierNode>(getIdentifier(node))) {
    validateDeclarationName(Decl::Kind::FunctionExprName, name);
  }

  // Set to false if the parameter list contains binding patterns.
  bool simpleParameterList = true;
  // All parameter identifiers.
  llvm::SmallVector<IdentifierNode *, 4> paramIds{};
  for (auto &param : params) {
    simpleParameterList &= !isa<PatternNode>(param);
    extractDeclaredIdentsFromID(&param, paramIds);
  }

  if (!simpleParameterList && useStrictNode) {
    sm_.error(
        useStrictNode->getSourceRange(),
        "'use strict' not allowed inside function with non-simple parameter list");
  }

  // Whether parameters must be unique.
  bool const uniqueParams = !simpleParameterList || curFunction()->strictMode ||
      isa<ArrowFunctionExpressionNode>(node);

  // Declare the parameters
  for (IdentifierNode *paramId : paramIds) {
    validateDeclarationName(Decl::Kind::Parameter, paramId);

    auto *paramDecl = semData_.newDecl(paramId->_name, Decl::Kind::Parameter);
    paramId->decl = paramDecl;
    Binding *prevName = bindingTable_.find(paramId->_name);
    if (prevName && prevName->decl->scope == semData_.getCurScope()) {
      if (uniqueParams) {
        sm_.error(
            paramId->getSourceRange(),
            "cannot declare two parameters with the same name '" +
                paramId->_name->str() + "'");
      }

      // Update the name binding to point to the latest declaration.
      prevName->decl = paramDecl;
      prevName->node = paramId;
    } else {
      bindingTable_.insert(paramId->_name, Binding{paramDecl, paramId});
    }

#if 0 // Not needed for now.
    newFuncCtx.semInfo->paramNames.push_back(paramDecl);
#endif
  }

  // Do not visit the identifier node, because that would try to resolve it
  // in an incorrect scope!
  // visitESTreeNode(*this, getIdentifier(node), node);

  // Visit the parameters before we have hoisted the body declarations.
  {
    llvm::SaveAndRestore<bool> oldIsFormalParams{isFormalParams_, true};
    for (auto &param : getParams(node))
      visitESTreeNode(*this, &param, node);
  }

  if (blockStmt) {
    if (!curFunction()->strictMode)
      promoteScopedFuncDecls(astContext_, kw_, node);
    processDeclarationsInScope(blockStmt);
  }

  // Finally visit the body.
  visitESTreeNode(*this, getBody(node), node);
}

Node *SemanticValidator::scanDirectivePrologue(NodeList &body) {
  Node *result = nullptr;
  for (auto &nodeRef : body) {
    auto *exprSt = dyn_cast<ESTree::ExpressionStatementNode>(&nodeRef);
    if (!exprSt || !exprSt->_directive)
      break;

    auto *directive = exprSt->_directive;

    if (directive == kw_.identUseStrict) {
      curFunction()->strictMode = true;
      if (!result)
        result = &nodeRef;
    }
  }

  return result;
}

bool SemanticValidator::matchLValue(hermes::ESTree::Node const *node) const {
  return isa<MemberExpressionNode>(node) || isa<IdentifierNode>(node);
}

void SemanticValidator::validateLValue(hermes::ESTree::Node const *node) const {
  if (isa<MemberExpressionNode>(node))
    return;
  if (!isa<IdentifierNode>(node)) {
    sm_.error(node->getSourceRange(), "invalid assignment left-hand side");
    return;
  }

  auto *idNode = cast<IdentifierNode>(node);

  /// 'arguments' cannot be modified in strict mode, but we also don't
  /// support modifying it in non-strict mode yet.
  if (idNode->_name == kw_.identArguments) {
    sm_.error(node->getSourceRange(), "assignment to 'arguments'");
    return;
  }

  // 'eval' cannot be used as a variable in strict mode. If it is disabled we
  // we don't report an error because it will be reported separately.
  if (idNode->_name == kw_.identEval && curFunction()->strictMode &&
      astContext_.getEnableEval()) {
    sm_.error(node->getSourceRange(), "assignment to 'eval'");
    return;
  }

  // If the identifier wasn't resolved, we must have printed an error already.
  if (!idNode->decl)
    return;

  if (idNode->decl->kind == Decl::Kind::Const) {
    sm_.error(node->getSourceRange(), "assignment to constant variable");
    return;
  }

  if (idNode->decl->kind == Decl::Kind::FunctionExprName) {
    if (curFunction()->strictMode) {
      sm_.error(
          node->getSourceRange(),
          "assignment to read-only function expression name");
    } else {
      sm_.warning(
          node->getSourceRange(),
          "assignment to read-only function expression name");
    }
    return;
  }
}

void SemanticValidator::validateAssignmentTarget(const Node *node) {
  if (isa<EmptyNode>(node))
    return;
  if (matchLValue(node)) {
    validateLValue(node);
    return;
  }

  if (auto *assign = dyn_cast<AssignmentPatternNode>(node)) {
    return validateAssignmentTarget(assign->_left);
  }

  if (auto *APN = dyn_cast<ArrayPatternNode>(node)) {
    for (auto &elem : APN->_elements) {
      validateAssignmentTarget(&elem);
    }
    return;
  }

  if (auto *RP = dyn_cast<RestElementNode>(node)) {
    return validateAssignmentTarget(RP->_argument);
  }

  if (auto *obj = dyn_cast<ObjectPatternNode>(node)) {
    for (auto &propNode : obj->_properties) {
      if (auto *prop = dyn_cast<PropertyNode>(&propNode)) {
        assert(
            prop->_kind->str() == "init" &&
            "getters and setters must have been reported by the parser");
        validateAssignmentTarget(prop->_value);
      } else {
        auto *rest = cast<RestElementNode>(&propNode);
        validateAssignmentTarget(rest->_argument);
      }
    }
    return;
  }

  sm_.error(node->getSourceRange(), "invalid assignment left-hand side");
}

void SemanticValidator::updateNodeStrictness(FunctionLikeNode *node) {
  auto strictness = ESTree::makeStrictness(curFunction()->strictMode);
  // Only verify the strictness if there are no errors. Otherwise it is not
  // possible to ensure that it is correct.
  assert(
      (sm_.getErrorCount() || !strictnessIsPreset_ ||
       node->strictness == strictness) &&
      "Preset strictness is different from detected strictness");
  node->strictness = strictness;
}

LabelDecorationBase *SemanticValidator::getLabelDecorationBase(
    StatementNode *node) {
  if (auto *LS = dyn_cast<LoopStatementNode>(node))
    return LS;
  if (auto *SS = dyn_cast<SwitchStatementNode>(node))
    return SS;
  if (auto *BS = dyn_cast<BreakStatementNode>(node))
    return BS;
  if (auto *CS = dyn_cast<ContinueStatementNode>(node))
    return CS;
  if (auto *LabS = dyn_cast<LabeledStatementNode>(node))
    return LabS;
  llvm_unreachable("invalid node type");
  return nullptr;
}

void SemanticValidator::resolveIdentifier(
    IdentifierNode *identifier,
    bool inTypeof) {
  Decl *decl = identifier->decl;
  if (!decl) {
    if (Binding *name = bindingTable_.find(identifier->_name))
      identifier->decl = decl = name->decl;
  }

  if (identifier->_name == kw_.identArguments) {
    if (!decl || decl->scope->parentFunction != semData_.getCurFunction()) {
      identifier->decl = &semData_.getCurFunction()->argumentsDecl;
      curFunction()->semInfo->usesArguments = true;
    }
    return;
  }
  if (decl)
    return;

  if (funcCtx_->strictMode) {
    UniqueString *funcName = funcCtx_->getFunctionName();

    sm_.warning(
        Warning::UndefinedVariable,
        identifier->getSourceRange(),
        Twine("the variable \"") + identifier->_name->str() +
            "\" was not declared in function \"" +
            (funcName ? funcName->str() : "global") + "\"");
  }

  // Declare an ambient global property.
  identifier->decl = decl = semData_.newGlobal(
      identifier->_name, Decl::Kind::UndeclaredGlobalProperty);

  bindingTable_.insertIntoScope(
      globalScope_, identifier->_name, Binding{decl, nullptr});
}

//===----------------------------------------------------------------------===//
// SemanticValidator::BindingTableScopeTy

SemanticValidator::ScopeRAII::ScopeRAII(SemanticValidator *sm)
    : semData_(&sm->semData_), bindingScope_(sm->bindingTable_) {
  semData_->pushScope();
}
SemanticValidator::ScopeRAII::ScopeRAII(SemanticValidator *sm, DontPush)
    : semData_(nullptr), bindingScope_(sm->bindingTable_) {}
SemanticValidator::ScopeRAII::~ScopeRAII() {
  if (semData_)
    semData_->popScope();
}

//===----------------------------------------------------------------------===//
// FunctionContext

FunctionContext::FunctionContext(
    SemanticValidator *validator,
    bool strictMode,
    FunctionLikeNode *node,
    FunctionInfo *aSemInfo)
    : validator_(validator),
      oldContextValue_(validator->funcCtx_),
      semInfo(
          aSemInfo != nullptr ? aSemInfo : validator->semData_.pushFunction()),
      popAtExit(aSemInfo == nullptr),
      strictMode(strictMode),
      node(node) {
  validator->funcCtx_ = this;

  if (node)
    node->setSemInfo(semInfo);
}

FunctionContext::~FunctionContext() {
  assert(
      validator_->semData_.getCurFunction() == semInfo &&
      "FunctionContext out of sync with SemContext");
  // If not the global function, pop it.
  if (popAtExit)
    validator_->semData_.popFunction();
  validator_->funcCtx_ = oldContextValue_;
}

UniqueString *FunctionContext::getFunctionName() const {
  if (node) {
    if (auto *idNode = dyn_cast_or_null<IdentifierNode>(getIdentifier(node)))
      return idNode->_name;
  }
  return nullptr;
}

} // namespace sem
} // namespace hermes
