/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/SemValidate.h"

#include "SemanticValidator.h"

#include "hermes/AST/ES5FindDecls.h"
#include "hermes/Support/PerfSection.h"

#include <map>

using namespace hermes::ESTree;

namespace hermes {
namespace sem {

using llvm::dyn_cast;

#ifndef NDEBUG
static llvm::FormattedString ind(unsigned level) {
  return llvm::left_justify("", level * 4);
}
#endif

void Decl::dump(unsigned level) const {
#ifndef NDEBUG
  llvm::outs() << ind(level) << "Decl '" << name << "' ";
  const char *s;
#define CASE(x) \
  case Kind::x: \
    s = #x;     \
    break;
  switch (kind) {
    CASE(Let)
    CASE(Const)
    CASE(Class)
    CASE(Import)
    CASE(ES5Catch)
    CASE(FunctionExprName)
    CASE(ScopedFunction)
    CASE(Var)
    CASE(Parameter)
    CASE(GlobalProperty)
    CASE(UndeclaredGlobalProperty)
  }
  llvm::outs() << s;
#undef CASE
#define CASE(x)    \
  case Special::x: \
    s = #x;        \
    break;
  switch (special) {
    CASE(NotSpecial)
    CASE(Arguments)
    CASE(Eval)
  }
#undef CASE
  if (functionInScope)
    llvm::outs() << " functionInScope";
  llvm::outs() << "\n";
#endif
}

void LexicalScope::dump(const SemData *sd, unsigned int level) const {
#ifndef NDEBUG
  llvm::outs() << ind(level)
               << (sd && sd->getCurScope() == this ? "(cur) " : "") << "Scope "
               << llvm::format("%p", this) << "\n";
  for (const auto &decl : decls) {
    decl->dump(level + 1);
  }
  for (const auto *fd : hoistedFunctions) {
    llvm::outs() << ind(level + 1) << "hoistedFunction "
                 << cast<IdentifierNode>(fd->_id)->_name->str() << "\n";
  }
#endif
}

void FunctionInfo::dump(const SemData *sd, unsigned level) const {
#ifndef NDEBUG
  llvm::outs() << ind(level)
               << (sd && sd->getCurFunction() == this ? "(cur) " : "")
               << "Func\n";
  std::map<const LexicalScope *, llvm::SmallVector<const LexicalScope *, 2>>
      children;

  for (const auto *sc : scopes) {
    if (sc == scopes[0])
      continue;
    children[sc->parentScope].push_back(sc);
  }

  unsigned processedCount = 0;
  std::function<void(const LexicalScope *, unsigned)> dumpScope =
      [&dumpScope, &children, &processedCount, sd](
          const LexicalScope *sc, unsigned level) {
        sc->dump(sd, level);
        ++processedCount;
        auto it = children.find(sc);
        if (it == children.end())
          return;
        for (auto *childScope : it->second)
          dumpScope(childScope, level + 1);
      };

  dumpScope(scopes[0], level + 1);
  assert(processedCount == scopes.size() && "not all scopes were visited");
#endif
}

void SemData::dump() const {
#ifndef NDEBUG
  llvm::outs() << "SemData\n";
  std::map<const FunctionInfo *, llvm::SmallVector<const FunctionInfo *, 2>>
      children;

  for (const auto &gd : globals_) {
    // Skip the ambient decls for brevity.
    if (gd.kind == Decl::Kind::UndeclaredGlobalProperty)
      continue;
    gd.dump(1);
  }

  for (const auto &F : functions_) {
    if (&F == &functions_[0])
      continue;
    children[F.parentFunction].push_back(&F);
  }

  unsigned processedCount = 0;
  std::function<void(const FunctionInfo *, unsigned)> dumpFunction =
      [&dumpFunction, &children, &processedCount, this](
          const FunctionInfo *F, unsigned level) {
        F->dump(this, level);
        ++processedCount;
        auto it = children.find(F);
        if (it == children.end())
          return;
        for (auto *childFunc : it->second)
          dumpFunction(childFunc, level + 1);
      };

  dumpFunction(&functions_[0], 0);
  assert(processedCount == functions_.size() && "not all scopes were visited");
#endif
}

SemData::SemData(Context &ctx)
    : identArguments_(ctx.getIdentifier("arguments")) {
  decls_.emplace_back(
      ctx.getIdentifier("eval"),
      Decl::Kind::UndeclaredGlobalProperty,
      Decl::Special::Eval);
  evalDecl_ = &decls_.back();

  // Create the global scope.
  pushFunction();
  pushScope();
}

SemData::~SemData() = default;

void SemData::processLibraryDeclarations(
    NameSet &names,
    hermes::DeclarationFileListTy const &declFileList) {
  auto declareAmbientProperty = [this, &names](Node *id) {
    auto *idNode = dyn_cast<IdentifierNode>(id);
    if (!idNode)
      return;
    // Skip duplicates.
    if (!names.insert(idNode->_name).second)
      return;

    newGlobal(idNode->_name, Decl::Kind::UndeclaredGlobalProperty);
  };

  for (auto *node : declFileList) {
    auto *program = dyn_cast<ProgramNode>(node);
    if (!program)
      continue;

    ESTree::ES5FindDecls DH;
    program->visit(DH);

    // Create variable declarations for each of the hoisted variables.
    for (auto vd : DH.decls)
      declareAmbientProperty(vd->_id);
    for (auto fd : DH.closures)
      declareAmbientProperty(fd->_id);
  }
}

FunctionInfo *SemData::pushFunction() {
  functions_.emplace_back(curFunction_, curScope_, identArguments_);
  return curFunction_ = &functions_.back();
}

void SemData::popFunction() {
  curFunction_ = curFunction_->parentFunction;
}

LexicalScope *SemData::pushScope() {
  scopes_.emplace_back(curFunction_, curScope_);
  auto *res = curScope_ = &scopes_.back();
  curFunction_->scopes.push_back(curScope_);
  return res;
}

void SemData::popScope() {
  curScope_ = curScope_->parentScope;
}

Decl *SemData::newDecl(UniqueString *name, Decl::Kind kind) {
  return newDeclInScope(name, kind, curScope_);
}

Decl *SemData::newDeclInScope(
    UniqueString *name,
    Decl::Kind kind,
    LexicalScope *scope) {
  assert(!Decl::isKindGlobal(kind) && "invalid non-global declaration kind");
  decls_.emplace_back(Identifier::getFromPointer(name), kind, scope);
  auto res = &decls_.back();
  scope->decls.push_back(res);
  return res;
}

Decl *SemData::newGlobal(
    hermes::UniqueString *name,
    hermes::sem::Decl::Kind kind) {
  assert(Decl::isKindGlobal(kind) && "invalid global declaration kind");
  globals_.emplace_back(
      Identifier::getFromPointer(name), kind, getGlobalScope());
  return &globals_.back();
}

void SemData::detachFromAST() {
  for (auto &F : functions_)
    F.imports.clear();
  for (auto &S : scopes_)
    S.hoistedFunctions.clear();
}

SemContext::SemContext(
    Context &ctx,
    hermes::DeclarationFileListTy const &declFileList)
    : ctx_(ctx), data_(std::make_shared<SemData>(ctx)) {
  SemData::NameSet names{};
  data_->processLibraryDeclarations(names, declFileList);
}

SemContext::SemContext(Context &ctx, const std::shared_ptr<SemData> &semData)
    : ctx_(ctx), data_(semData) {}

SemContext::~SemContext() {
  if (ctx_.isLazyCompilation())
    data_->detachFromAST();
}

bool validateAST(
    Context &astContext,
    SemContext &semCtx,
    ProgramNode *root,
    bool global) {
  PerfSection validation("Validating JavaScript function AST");
  // Validate the entire AST.
  SemanticValidator validator{astContext, semCtx, nullptr};
  return validator.doIt(root, global);
}

bool validateFunctionAST(
    Context &astContext,
    SemContext &semCtx,
    Node *function,
    ESTree::Strictness strict) {
  PerfSection validation("Validating JavaScript function AST: Deep");
  SemanticValidator validator{astContext, semCtx, nullptr};
  return validator.doFunction(
      function, strict == ESTree::Strictness::StrictMode);
}

} // namespace sem
} // namespace hermes
