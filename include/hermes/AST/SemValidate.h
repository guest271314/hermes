/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_AST_SEMVALIDATE_H
#define HERMES_AST_SEMVALIDATE_H

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/Support/SourceErrorManager.h"

#include <deque>

namespace hermes {
namespace sem {

class Decl;
class LexicalScope;
class FunctionInfo;
class SemData;

class Decl {
 public:
  enum class Kind : uint8_t {
    // ==== Let-like declarations ===
    Let,
    Const,
    Class,
    Import,
    /// A single catch variable declared like this "catch (e)", see
    /// ES10 B.3.5 VariableStatements in Catch Blocks
    ES5Catch,

    // ==== other declarations ===
    FunctionExprName,
    /// Function declaration visible only in its lexical scope.
    ScopedFunction,

    // ==== Var-like declarations ===

    /// "var" in function scope.
    Var,
    Parameter,
    /// "var" in global scope.
    GlobalProperty,
    UndeclaredGlobalProperty,
  };

  enum class Special : uint8_t {
    NotSpecial,
    Arguments,
    Eval,
  };

  /// \return true if this kind of declaration is function scope (and can be
  /// re-declared).
  static bool isKindVarLike(Kind kind) {
    return kind >= Kind::Var;
  }
  static bool isKindVarLikeOrScopedFunction(Kind kind) {
    return kind >= Kind::ScopedFunction;
  }
  /// \return true if this kind of declaration is lexically scoped (and cannot
  /// be re-declared).
  static bool isKindLetLike(Kind kind) {
    return kind <= Kind::ES5Catch;
  }
  /// \return true if this kind of declaration is a global property.
  static bool isKindGlobal(Kind kind) {
    return kind >= Kind::GlobalProperty;
  }

  /// Identifier that is declared.
  Identifier const name;
  /// What kind of declaration it is.
  Kind kind;
  /// If this is a special declaration, identify which one.
  Special const special;
  /// Set to true if this is a function initialized in this scope. It helps
  /// IRGen avoid initializing the variable twice.
  bool functionInScope = false;

  /// The lexical scope of the declaration. Could be nullptr for special
  /// declarations, since they are technically unscoped.
  LexicalScope *const scope;

  Decl(Identifier name, Kind kind, LexicalScope *scope)
      : name(name), kind(kind), special(Special::NotSpecial), scope(scope) {}
  Decl(Identifier name, Kind kind, Special special)
      : name(name), kind(kind), special(special), scope(nullptr) {}

  void dump(unsigned level = 0) const;
};

class LexicalScope {
 public:
  /// The function owning this lexical scope.
  FunctionInfo *const parentFunction{};
  /// The enclosing lexical scope (it could be in another function).
  LexicalScope *const parentScope{};

  /// All declarations made in this scope.
  llvm::SmallVector<Decl *, 2> decls{};

  /// A list of functions that need to be hoisted and materialized before we
  /// can generate the rest of the scope.
  llvm::SmallVector<ESTree::FunctionDeclarationNode *, 2> hoistedFunctions{};

  LexicalScope(FunctionInfo *parentFunction, LexicalScope *parentScope)
      : parentFunction(parentFunction), parentScope(parentScope) {}

  void dump(const SemData *sd = nullptr, unsigned level = 0) const;
};

class FunctionInfo {
 public:
  /// The function surrounding this function.
  FunctionInfo *const parentFunction;
  /// The enclosing lexical scope.
  LexicalScope *const parentScope;
  /// All lexical scopes in this function. The first one is the function scope.
  llvm::SmallVector<LexicalScope *, 4> scopes{};
  /// The implicitly declared "arguments" object.
  Decl argumentsDecl;

#if 0 // Not needed for now.
  /// Parameter names.
  llvm::SmallVector<Decl *, 4> paramNames{};
#endif

  /// A list of imports that need to be hoisted and materialized before we
  /// can generate the rest of the scope.
  /// Any line of the scope may use the imported values.
  llvm::SmallVector<ESTree::ImportDeclarationNode *, 2> imports{};

  /// Whether this function references the "arguments" special object.
  bool usesArguments = false;

  /// Whether this function contains arrow functions.
  bool containsArrowFunctions = false;

  /// This is a logical or of the \c usesArguments flags of all contained
  /// arrow functions. This will be used as a conservative estimate of
  /// whether a non-arrow function needs to eagerly create and capture its
  /// Arguments object.
  bool containsArrowFunctionsUsingArguments = false;

  /// Number of labels allocated so far. We use this counter to assign
  /// consecutive index values to labels.
  unsigned labelCount = 0;

  /// Allocate a new label and return its index.
  unsigned allocateLabel() {
    return labelCount++;
  }
  FunctionInfo(
      FunctionInfo *parentFunction,
      LexicalScope *parentScope,
      Identifier identArguments)
      : parentFunction(parentFunction),
        parentScope(parentScope),
        argumentsDecl(
            identArguments,
            Decl::Kind::Var,
            Decl::Special::Arguments) {}

  LexicalScope *getFunctionScope() const {
    return scopes[0];
  }

  void dump(const SemData *sd = nullptr, unsigned level = 0) const;
};

class SemData {
 public:
  SemData(Context &ctx);
  ~SemData();

  /// Create a new function and make it "current".
  FunctionInfo *pushFunction();
  /// Set the current function to current function's parent function.
  void popFunction();
  /// Create a new lexical scope, make it "current" and return it.
  LexicalScope *pushScope();
  /// Set the current lexical scope to current scopes's parent scope.
  void popScope();
  /// Create a new declaration in the current lexical scope.
  Decl *newDecl(UniqueString *name, Decl::Kind kind);
  /// Create a new declaration in the specified lexical scope.
  Decl *
  newDeclInScope(UniqueString *name, Decl::Kind kind, LexicalScope *scope);

  /// Create a new global property.
  Decl *newGlobal(UniqueString *name, Decl::Kind kind);

  /// Reset the current function and scope to the global ones.
  void resetToGlobal() {
    curFunction_ = getGlobalFunction();
    curScope_ = getGlobalScope();
  }

  /// Set the current scope to the specified one and set the current function
  /// to the scopes function.
  /// \param scope The scope to set. If nullptr, global scope is assumed.
  void setCurScope(LexicalScope *scope) {
    if (!scope) {
      resetToGlobal();
    } else {
      curFunction_ = scope->parentFunction;
      curScope_ = scope;
    }
  }

  /// \return the current function.
  FunctionInfo *getCurFunction() const {
    return curFunction_;
  }

  /// \return the current lexical scope.
  LexicalScope *getCurScope() const {
    return curScope_;
  }

  /// \return the global function
  FunctionInfo *getGlobalFunction() {
    return &functions_.at(0);
  }
  /// \return the global lexical scope.
  LexicalScope *getGlobalScope() {
    return &scopes_.at(0);
  }

  /// Return the special global "eval" declaration.
  Decl *getEvalDecl() {
    assert(evalDecl_ && "evalDecl must be initialized");
    return evalDecl_;
  }

  /// An iterator over global property declarations.
  using GlobalIterator = std::deque<Decl>::iterator;

  /// Return all global property declarations.
  llvm::iterator_range<GlobalIterator> getGlobals() {
    return {globals_.begin(), globals_.end()};
  }

  /// Invoke the callback F with each scope starting from the root and up to and
  /// including the specified top scope \p top. Uses memory proportional to
  /// the scope depth.
  /// \param top the specifed top scope. Nullptr is an alias for the global
  ///     scope.
  template <typename F>
  void forEachScopeUpTo(LexicalScope *top, const F &f) {
    // nullptr scope means the global scope.
    if (!top)
      top = getGlobalScope();

    // Collect the scopes in reverse order (starting from current).
    llvm::SmallVector<LexicalScope *, 4> lexicalChain{};
    for (auto *sc = top; sc; sc = sc->parentScope)
      lexicalChain.push_back(sc);

    for (auto it = lexicalChain.rbegin(), e = lexicalChain.rend(); it != e;
         ++it) {
      LexicalScope *sc = *it;
      f(sc);
    }
  }

  /// An identifier set used to prevent adding more than one global with the
  /// same name durinng initialization.
  using NameSet = llvm::SmallDenseSet<UniqueString *, 1>;

  /// Declare all library symbols in the global scope.
  /// \param names a set of identifiers to prevent adding the same global
  ///     more than once.
  void processLibraryDeclarations(
      NameSet &names,
      const DeclarationFileListTy &declFileList);

  /// Remove all links to AST nodes.
  void detachFromAST();

  void dump() const;

 private:
  /// The "arguments" identifier.
  Identifier identArguments_;
  /// The special global "eval" declaration.
  Decl *evalDecl_;

  FunctionInfo *curFunction_ = nullptr;
  LexicalScope *curScope_ = nullptr;

  /// Global properties.
  std::deque<Decl> globals_{};
  std::deque<Decl> decls_{};
  std::deque<LexicalScope> scopes_{};
  std::deque<FunctionInfo> functions_{};
};

/// Identifier and label tables, populated by the semantic validator. They need
/// to be stored separately from the AST because they have destructors, while
/// the AST is stored in a pool.
class SemContext {
 public:
  SemContext(Context &ctx, const DeclarationFileListTy &declFileList);
  SemContext(Context &ctx, const std::shared_ptr<SemData> &semData);
  ~SemContext();

  /// An iterator over global property declarations.
  using GlobalIterator = SemData::GlobalIterator;

  /// Return all global property declarations.
  llvm::iterator_range<GlobalIterator> getGlobals() {
    return getData().getGlobals();
  }

  SemData &getData() {
    return *data_;
  }

  const std::shared_ptr<SemData> &getDataPtr() const {
    return data_;
  }

 private:
  Context &ctx_;
  std::shared_ptr<SemData> data_;
};

/// Perform semantic validation of the entire AST, starting from the specified
/// root, which should be ProgramNode.
/// \param global if true, validate the node in global scope.
bool validateAST(
    Context &astContext,
    SemContext &semCtx,
    ESTree::ProgramNode *root,
    bool global);

/// Perform semantic validation of an individual function in the given context
/// \param function must be a function node
/// \param strict specifies parent strictness.
bool validateFunctionAST(
    Context &astContext,
    SemContext &semCtx,
    ESTree::NodePtr function,
    ESTree::Strictness strict);

/// Perform semantic validation of an individual function in the given context
/// \param function must be a function node
/// \param strict specifies parent strictness.
bool validateLazyFunctionAST(
    Context &astContext,
    SemContext &semCtx,
    LexicalScope *lexicalScope,
    ESTree::NodePtr function,
    ESTree::Strictness strict);

} // namespace sem
} // namespace hermes

#endif // HERMES_AST_SEMVALIDATE_H
