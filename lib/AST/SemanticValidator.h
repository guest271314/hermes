/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_AST_SEMANTICVALIDATOR_H
#define HERMES_AST_SEMANTICVALIDATOR_H

#include "hermes/ADT/ScopedHashTable.h"
#include "hermes/AST/SemValidate.h"

#include "RecursiveVisitor.h"
#include "ValidatorHelpers.h"

namespace hermes {
namespace sem {

using namespace hermes::ESTree;

// Forward declarations
class FunctionContext;
class SemanticValidator;

//===----------------------------------------------------------------------===//
// SemanticValidator

/// Class the performs all semantic validation
class SemanticValidator {
  friend class FunctionContext;

  /// Binding between an identifier and its declaration in a scope.
  struct Binding {
    Decl *decl = nullptr;
    /// The declaring node. Note that this is nullable.
    Node *node = nullptr;

    Binding() = default;
    Binding(Decl *decl, Node *node) : decl(decl), node(node) {}

    bool isValid() const {
      return decl != nullptr;
    }
    void invalidate() {
      decl = nullptr;
      node = nullptr;
    }
  };

  /// The scoped binding table mapping from string to binding.
  using BindingTableTy = hermes::ScopedHashTable<UniqueString *, Binding>;
  using BindingTableScopeTy =
      hermes::ScopedHashTableScope<UniqueString *, Binding>;

  /// A RAII object automatic scopes.
  /// On construction it creates a new binding table scope and pushes a new
  /// semantic scope.
  /// Om destruction it destroys the binding scope and pops the semantic scope.
  class ScopeRAII {
   public:
    /// A tag type indicating that we shouldn't push a semantic scope.
    struct DontPush {};

    /// Create a binding scope and push a semantic scope.
    explicit ScopeRAII(SemanticValidator *sm);

    /// Create the global binding scope without pushing a semantic scope.
    explicit ScopeRAII(SemanticValidator *sm, DontPush);

    ~ScopeRAII();

    BindingTableScopeTy &getBindingScope() {
      return bindingScope_;
    }

   private:
    /// The semantic context. If non-null, pop a scope on destruction.
    SemData *const semData_;
    /// The binding table scope.
    BindingTableScopeTy bindingScope_;
  };

  Context &astContext_;
  /// A copy of Context::getSM() for easier access.
  SourceErrorManager &sm_;

  /// Buffer all generated messages and print them sorted in the end.
  SourceErrorManager::SaveAndBufferMessages bufferMessages_;

  /// All semantic tables are persisted here.
  SemData &semData_;

  /// Save the initial error count so we know whether we generated any errors.
  const unsigned initialErrorCount_;

  /// Keywords we will be checking for.
  Keywords kw_;

  /// The current function context.
  FunctionContext *funcCtx_{};

  /// True if we are validating a formal parameter list.
  bool isFormalParams_{false};

  /// The currently lexically visible names.
  BindingTableTy bindingTable_{};

  std::deque<ScopeRAII> scopes_{};

  /// The global scope.
  BindingTableScopeTy *globalScope_ = nullptr;

#ifndef NDEBUG
  /// Our parser detects strictness and initializes the flag in every node,
  /// but if we are reading an external AST, we must look for "use strict" and
  /// initialize the flag ourselves here.
  /// For consistency we always perform the detection, but in debug mode we also
  /// want to ensure that our results match what the parser generated. This
  /// flag indicates whether strictness is preset or not.
  bool strictnessIsPreset_{false};
#endif

 public:
  explicit SemanticValidator(
      Context &astContext,
      sem::SemContext &semCtx,
      sem::LexicalScope *lexicalScope);

  ~SemanticValidator();

  // Perform the validation on whole AST.
  /// \param global if true, validate the node in global scope.
  bool doIt(ProgramNode *rootNode, bool global);

  /// Perform the validation on an individual function.
  bool doFunction(Node *function, bool strict);

  /// Handle the default case for all nodes which we ignore, but we still want
  /// to visit their children.
  void visit(Node *node) {
    visitESTreeChildren(*this, node);
  }

  void visit(FunctionDeclarationNode *funcDecl);
  void visit(FunctionExpressionNode *funcExpr);
  void visit(ArrowFunctionExpressionNode *arrowFunc);

  void visit(BlockStatementNode *blockStmt, Node *parent);

  void visit(MetaPropertyNode *metaProp);
  void visit(IdentifierNode *identifier, Node *parent);

  void visit(ForInStatementNode *forIn);
  void visit(ForOfStatementNode *forOf);
  /// Visit a for-of or for-in statement.
  void visitForInOf(
      LoopStatementNode *loopNode,
      Node *left,
      Node *right,
      Node *body,
      ScopeDecorationBase *scopeDecoration);

  void visit(AssignmentExpressionNode *assignment);
  void visit(UpdateExpressionNode *update);

  void visit(LabeledStatementNode *labelStmt);

  void visit(RegExpLiteralNode *regexp);

  void visit(TryStatementNode *tryStatement);
  void visit(CatchClauseNode *catchClause);

  void visit(DoWhileStatementNode *loop);
  void visit(ForStatementNode *loop);
  void visit(WhileStatementNode *loop);
  void visit(SwitchStatementNode *switchStmt);

  void visit(BreakStatementNode *breakStmt);
  void visit(ContinueStatementNode *continueStmt);

  void visit(ReturnStatementNode *returnStmt);
  void visit(YieldExpressionNode *yieldExpr);

  void visit(CallExpressionNode *callExpr);

  void visit(UnaryExpressionNode *unaryExpr);

  void visit(ArrayPatternNode *arrayPat);

  void visit(SpreadElementNode *S, Node *parent);

  void visit(ClassExpressionNode *node);
  void visit(ClassDeclarationNode *node);

  void visit(VariableDeclarationNode *varDecl);

  void visit(ImportDeclarationNode *importDecl);

  void visit(ExportNamedDeclarationNode *exportDecl);
  void visit(ExportDefaultDeclarationNode *exportDecl);
  void visit(ExportAllDeclarationNode *exportDecl);

  void visit(CoverEmptyArgsNode *CEA);
  void visit(CoverTrailingCommaNode *CTC);
  void visit(CoverInitializerNode *CI);
  void visit(CoverRestElementNode *R);

 private:
  inline bool haveActiveContext() const {
    return funcCtx_ != nullptr;
  }

  inline FunctionContext *curFunction() {
    assert(funcCtx_ && "No active function context");
    return funcCtx_;
  }
  inline const FunctionContext *curFunction() const {
    assert(funcCtx_ && "No active function context");
    return funcCtx_;
  }

  /// Start the validation process by visiting a ProgramNode.
  /// \param global whether to validate in global or local scope.
  void visitProgram(ProgramNode *node, bool global);

  /// Import all global declarations into the name table.
  void declareGlobals();

  /// Declare all declarations from \p declList in the current scope by calling
  /// \c validateAndDeclareIdentifier().
  void processDeclarationsInScope(ScopeDecorationBase *astScope);

  /// Extract the list of declared identifiers in a declaration node and return
  /// the declaration kind of the node. The declaration kind is adjusted
  /// depending on the scope.
  Decl::Kind extractDeclaredIdents(
      Node *declarationNode,
      llvm::SmallVectorImpl<IdentifierNode *> &idents);

  /// Extract the declared identifiers from a declaration AST node's "id" field.
  /// Normally that is just a single identifier, but it can be more in case of
  /// destructuring.
  void extractDeclaredIdentsFromID(
      Node *node,
      llvm::SmallVectorImpl<IdentifierNode *> &idents);

  /// Try to create a declaration of the specified kind and name in the current
  /// scope. If the declaration is invalid, print an error message without
  /// creating it.
  /// \param declKind the semantic declaration kind
  /// \param idNode the AST node containing the name
  /// \param declNode the AST node of the declaration. Used for function
  ///     declarations.
  void validateAndDeclareIdentifier(
      Decl::Kind declKind,
      IdentifierNode *idNode,
      Node *declNode);

  /// Validate a var declaration when it is encountered. This catches the cases
  /// which couldn't be verified when entering the scope and processing the
  /// declaration list.
  void validateVarDeclaration(IdentifierNode *idNode);

  /// Ensure that the specified identifier is valid to be used in a declaration.
  /// Return true if valid, otherwise generate an error and return false.
  bool validateDeclarationName(
      Decl::Kind declKind,
      const IdentifierNode *idNode) const;

  /// Process a function declaration by creating a new FunctionContext. Update
  /// the context with the strictness of the function.
  /// \param node the current node
  /// \param params the parameter list
  /// \param body the body. It may be a BlockStatementNode, an EmptyNode (for
  ///     lazy functions), or an expression (for simple arrow functions).
  void visitFunction(FunctionLikeNode *node, NodeList &params, Node *body);

  /// Scan a list of directives in the beginning of a program of function
  /// (see ES5.1 4.1 - a directive is a statement consisting of a single
  /// string literal).
  /// Update the flags in the function context to reflect the directives. (We
  /// currently only recognize "use strict".)
  /// \return the node containing "use strict" or nullptr.
  Node *scanDirectivePrologue(NodeList &body);

  /// \return true of the node looks like an lvalue. It could still be invalid.
  bool matchLValue(const Node *node) const;

  /// Print an error if the node is not a valid assignable l-value.
  void validateLValue(const Node *node) const;

  /// Ensure that the specified node is a valid target for an assignment, in
  /// other words it is an l-value, a Pattern (checked recursively) or an Empty
  /// (used by elision).
  void validateAssignmentTarget(const Node *node);

  /// A debugging method to set the strictness of a function-like node to
  /// the curent strictness, asserting that it doesn't change if it had been
  /// preset.
  void updateNodeStrictness(FunctionLikeNode *node);

  /// Get the LabelDecorationBase depending on the node type.
  static LabelDecorationBase *getLabelDecorationBase(StatementNode *node);

  /// Resolve the variable in the name table. Generate a warning if it cannot
  /// be found and declare an ambient global property.
  /// \param inTypeof if true, this is an argument of 'typeof'. So some checks
  ///     are not performed.
  void resolveIdentifier(IdentifierNode *identifier, bool inTypeof);
};

//===----------------------------------------------------------------------===//
// FunctionContext

/// Holds all per-function state, specifically label tables. Should always be
/// constructed on the stack.
class FunctionContext {
  SemanticValidator *validator_;
  FunctionContext *oldContextValue_;

 public:
  struct Label {
    /// Where it was declared.
    IdentifierNode *declarationNode;

    /// Statement targeted by the label. It is either a LoopStatement or a
    /// LabeledStatement.
    StatementNode *targetStatement;
  };

  /// The associated seminfo object
  sem::FunctionInfo *const semInfo;

  /// Whether to pop the sem function on destruction.
  bool const popAtExit;

  /// Is this function in strict mode.
  bool strictMode = false;

  /// The most nested active loop statement.
  LoopStatementNode *activeLoop = nullptr;
  /// The most nested active loop or switch statement.
  StatementNode *activeSwitchOrLoop = nullptr;
  /// The AST node of the function.
  FunctionLikeNode *const node;

  /// The currently active labels in the function.
  llvm::DenseMap<NodeLabel, Label> labelMap;

  explicit FunctionContext(
      SemanticValidator *validator,
      bool strictMode,
      FunctionLikeNode *node,
      FunctionInfo *aSemInfo);

  ~FunctionContext();

  /// \return true if this is the "global scope" function context, in other
  /// words not a real function.
  bool isGlobalScope() const {
    return !oldContextValue_;
  }

  /// Allocate a new label in the current context.
  unsigned allocateLabel() {
    return semInfo->allocateLabel();
  }

  /// \return the optional function name, or nullptr.
  UniqueString *getFunctionName() const;
};

} // namespace sem
} // namespace hermes

#endif // HERMES_AST_SEMANTICVALIDATOR_H
