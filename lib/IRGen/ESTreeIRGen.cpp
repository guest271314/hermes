/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"

#include "llvh/ADT/StringSet.h"
#include "llvh/Support/Debug.h"
#include "llvh/Support/SaveAndRestore.h"

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// Free standing helpers.

/// \returns true if \p node is a constant expression.
bool isConstantExpr(ESTree::Node *node) {
  // TODO: a little more agressive constant folding.
  switch (node->getKind()) {
    case ESTree::NodeKind::StringLiteral:
    case ESTree::NodeKind::NumericLiteral:
    case ESTree::NodeKind::NullLiteral:
    case ESTree::NodeKind::BooleanLiteral:
      return true;
    default:
      return false;
  }
}

//===----------------------------------------------------------------------===//
// LexicalScopeRAII

LexicalScopeRAII::LexicalScopeRAII(ESTreeIRGen *irGen)
    : irGen_(irGen),
      nameTableScope_(irGen->nameTable_),
      currentIRScopeDesc_(irGen->currentIRScopeDesc_),
      currentIRScope_(irGen->currentIRScope_) {}

LexicalScopeRAII::~LexicalScopeRAII() {
  irGen_->currentIRScope_ = currentIRScope_;
  irGen_->currentIRScopeDesc_ = currentIRScopeDesc_;
}

void LexicalScopeRAII::bindGlobalProperty(
    Identifier name,
    GlobalObjectProperty *prop) {
  irGen_->nameTable_.insertIntoScope(&nameTableScope_, name, prop);
}

//===----------------------------------------------------------------------===//
// LReference

IRBuilder &LReference::getBuilder() {
  return irgen_->Builder;
}

Value *LReference::emitLoad() {
  auto &builder = getBuilder();
  IRBuilder::ScopedLocationChange slc(builder, loadLoc_);

  switch (kind_) {
    case Kind::Empty:
      assert(false && "empty cannot be loaded");
      return builder.getLiteralUndefined();
    case Kind::Member:
      return builder.createLoadPropertyInst(base_, property_);
    case Kind::VarOrGlobal:
      return irgen_->emitLoad(resolved_);
    case Kind::Destructuring:
      assert(false && "destructuring cannot be loaded");
      return builder.getLiteralUndefined();
    case Kind::Error:
      return builder.getLiteralUndefined();
  }

  llvm_unreachable("invalid LReference kind");
}

void LReference::emitStore(Value *value) {
  auto &builder = getBuilder();

  switch (kind_) {
    case Kind::Empty:
      return;
    case Kind::Member:
      builder.createStorePropertyInst(value, base_, property_);
      return;
    case Kind::VarOrGlobal:
      irgen_->emitStore(value, resolved_, declInit_);
      return;
    case Kind::Error:
      return;
    case Kind::Destructuring:
      return irgen_->emitDestructuringAssignment(
          declInit_, destructuringTarget_, value);
  }

  llvm_unreachable("invalid LReference kind");
}

bool LReference::canStoreWithoutSideEffects() const {
  return kind_ == Kind::VarOrGlobal && llvh::isa<ScopeVar>(base_) &&
      !cast<ScopeVar>(base_)->isReadOnly();
}

llvh::Optional<Identifier> LReference::getNameHint() const {
  if (kind_ != Kind::VarOrGlobal)
    return llvh::None;
  return resolved_.getName();
}

//===----------------------------------------------------------------------===//
// ESTreeIRGen

ESTreeIRGen::ESTreeIRGen(
    ESTree::Node *root,
    const DeclarationFileListTy &declFileList,
    Module *M,
    const ScopeChain &scopeChain)
    : Mod(M),
      Builder(Mod),
      instrumentIR_(M, Builder),
      Root(root),
      DeclarationFileList(declFileList),
      lexicalScopeChain(resolveScopeIdentifiers(scopeChain)),
      identEval_(Builder.createIdentifier("eval")),
      identLet_(Builder.createIdentifier("let")),
      identDefaultExport_(Builder.createIdentifier("?default")) {}

void ESTreeIRGen::doIt() {
  LLVM_DEBUG(dbgs() << "Processing top level program.\n");

  ESTree::ProgramNode *Program;

  Program = llvh::dyn_cast<ESTree::ProgramNode>(Root);

  if (!Program) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        SMLoc{}, "missing 'Program' AST node");
    return;
  }

  LLVM_DEBUG(dbgs() << "Found Program decl.\n");

  // The function which will "execute" the module.
  Function *topLevelFunction;

  LexicalScopeRAII saveGlobalScope(this);

  // Function context used only when compiling in an existing lexical scope
  // chain. It is only initialized if we have a lexical scope chain.
  llvh::Optional<FunctionContext> wrapperFunctionContext{};

  if (!lexicalScopeChain) {
    topLevelFunction = Builder.createTopLevelFunction(
        ESTree::isStrict(Program->strictness), Program->getSourceRange());

    setNewScope(
        topLevelFunction->createScopeDesc(nullptr, ScopeDesc::Kind::Global),
        Builder.getGlobalObject());
    bottomMostScopeDesc_ = currentIRScopeDesc_;
  } else {
    // If compiling in an existing lexical context, we need to install the
    // scopes in a wrapper function, which represents the "global" code.

    Function *wrapperFunction = Builder.createFunction(
        "",
        Function::DefinitionKind::ES5Function,
        ESTree::isStrict(Program->strictness),
        Program->getSourceRange(),
        true);

    setNewScope(
        wrapperFunction->createScopeDesc(nullptr, ScopeDesc::Kind::Global),
        Builder.getGlobalObject());
    bottomMostScopeDesc_ = currentIRScopeDesc_;

    // Initialize the wrapper context.
    wrapperFunctionContext.emplace(this, wrapperFunction, nullptr);

    // Populate it with dummy code so it doesn't crash the back-end.
    genDummyFunction(wrapperFunction);

    // Restore the previously saved parent scopes.
    materializeScopesInChain(wrapperFunction, lexicalScopeChain, -1);

    // Finally create the function which will actually be executed.
    topLevelFunction = Builder.createFunction(
        "eval",
        Function::DefinitionKind::ES5Function,
        ESTree::isStrict(Program->strictness),
        Program->getSourceRange(),
        false);
  }

  Mod->setTopLevelFunction(topLevelFunction);

  // Function context for topLevelFunction.
  FunctionContext topLevelFunctionContext{
      this, topLevelFunction, Program->getSemInfo()};

  // IRGen needs a pointer to the outer-most context, which is either
  // topLevelContext or wrapperFunctionContext, depending on whether the latter
  // was created.
  // We want to set the pointer to that outer-most context, but ensure that it
  // doesn't outlive the context it is pointing to.
  llvh::SaveAndRestore<FunctionContext *> saveTopLevelContext(
      topLevelContext,
      !wrapperFunctionContext.hasValue() ? &topLevelFunctionContext
                                         : &wrapperFunctionContext.getValue());

  // Now declare all externally supplied global properties, but only if we don't
  // have a lexical scope chain.
  if (!lexicalScopeChain) {
    for (auto declFile : DeclarationFileList) {
      processDeclarationFile(declFile);
    }
  }

  emitFunctionPrologue(
      Program,
      Builder.createBasicBlock(topLevelFunction),
      InitES5CaptureState::Yes,
      DoEmitParameters::Yes);

  Value *retVal;
  {
    // Allocate the return register, initialize it to undefined.
    curFunction()->globalReturnRegister =
        Builder.createAllocStackInst(genAnonymousLabelName("ret"));
    Builder.createStoreStackInst(
        Builder.getLiteralUndefined(), curFunction()->globalReturnRegister);

    genBody(Program->_body);

    // Terminate the top-level scope with a return statement.
    retVal = Builder.createLoadStackInst(curFunction()->globalReturnRegister);
  }

  emitFunctionEpilogue(retVal);
}

void ESTreeIRGen::doCJSModule(
    Function *topLevelFunction,
    sem::FunctionInfo *semInfo,
    uint32_t segmentID,
    uint32_t id,
    llvh::StringRef filename) {
  assert(Root && "no root in ESTreeIRGen");
  auto *func = cast<ESTree::FunctionExpressionNode>(Root);
  assert(func && "doCJSModule without a module");

  LexicalScopeRAII saveGlobalScope(this);

  // Find the global scope.
  assert(topLevelFunction && "CJS module compiled without an entry function");
  assert(
      topLevelFunction->isGlobalScope() &&
      "CJS module entry function is not global scope");
  // Find the global scope.
  ScopeDesc *globalScopeDesc = nullptr;
  for (auto *scopeDesc : topLevelFunction->getScopes()) {
    if (scopeDesc->isGlobalScope()) {
      globalScopeDesc = scopeDesc;
      break;
    }
  }
  assert(globalScopeDesc && "CJS module compiled without a global scope");

  setNewScope(globalScopeDesc, Builder.getGlobalObject());
  bottomMostScopeDesc_ = globalScopeDesc;

  FunctionContext topLevelFunctionContext{this, topLevelFunction, semInfo};
  llvh::SaveAndRestore<FunctionContext *> saveTopLevelContext(
      topLevelContext, &topLevelFunctionContext);

  // Now declare all externally supplied global properties, but only if we don't
  // have a lexical scope chain.
  assert(
      !lexicalScopeChain &&
      "Lexical scope chain not supported for CJS modules");
  for (auto declFile : DeclarationFileList) {
    processDeclarationFile(declFile);
  }

  Identifier functionName = Builder.createIdentifier("cjs_module");
  Function *newFunc = genES5Function(functionName, nullptr, func);

  Builder.getModule()->addCJSModule(
      segmentID, id, Builder.createIdentifier(filename), newFunc);
}

static int getDepth(const std::shared_ptr<SerializedScope> chain) {
  int depth = 0;
  const SerializedScope *current = chain.get();
  while (current) {
    depth += 1;
    current = current->parentScope.get();
  }
  return depth;
}

std::pair<Function *, Function *> ESTreeIRGen::doLazyFunction(
    hbc::LazyCompilationData *lazyData) {
  // Create a top level function that will never be executed, because:
  // 1. IRGen assumes the first function always has global scope
  // 2. It serves as the root for dummy functions for lexical data
  Function *topLevel = Builder.createTopLevelFunction(lazyData->strictMode, {});

  FunctionContext topLevelFunctionContext{this, topLevel, nullptr};

  // Save the top-level context, but ensure it doesn't outlive what it is
  // pointing to.
  llvh::SaveAndRestore<FunctionContext *> saveTopLevelContext(
      topLevelContext, &topLevelFunctionContext);

  auto *node = cast<ESTree::FunctionLikeNode>(Root);

  // We restore scoping information in two separate ways:
  // 1. By adding them to ExternalScopes for resolution here
  // 2. By adding dummy functions for lexical scoping debug info later
  //
  // Instruction selection determines the delta between the ExternalScope
  // and the dummy function chain, so we add the ExternalScopes with
  // positive depth.
  lexicalScopeChain = lazyData->parentScope;
  materializeScopesInChain(
      topLevel, lexicalScopeChain, getDepth(lexicalScopeChain) - 1);

  // If lazyData->closureAlias is specified, we must create an alias binding
  // between originalName (which must be valid) and the variable identified by
  // closureAlias.
  ScopeVar *parentVar = nullptr;
  if (lazyData->closureAlias.isValid()) {
    assert(lazyData->originalName.isValid() && "Original name invalid");
    assert(
        lazyData->originalName != lazyData->closureAlias &&
        "Original name must be different from the alias");

    // NOTE: the closureAlias target must exist and must be a Variable.
    parentVar = cast<ScopeVar>(nameTable_.lookup(lazyData->closureAlias));
  }

  assert(
      !llvh::isa<ESTree::ArrowFunctionExpressionNode>(node) &&
      "lazy compilation not supported for arrow functions");

  // Generators have had their lazy scope set up without setting one up
  // for the inner functions. This means that we will never directly generate
  // a GeneratorInnerFunction here.
  Function *func = ESTree::isAsync(node)
      ? genAsyncFunction(lazyData->originalName, parentVar, node)
      : ESTree::isGenerator(node)
      ? genGeneratorFunction(lazyData->originalName, parentVar, node)
      : genES5Function(lazyData->originalName, parentVar, node, false);
  addLexicalDebugInfo(func, topLevel, lexicalScopeChain);
  return {func, topLevel};
}

std::pair<Value *, bool> ESTreeIRGen::declareVariableOrGlobalProperty(
    Function *inFunc,
    VarDecl::Kind declKind,
    Identifier name) {
  Value *found = nameTable_.lookup(name);

  // If the variable is already declared in this scope, do not create a
  // second instance.
  if (found) {
    if (auto *var = llvh::dyn_cast<ScopeVar>(found)) {
      if (var->getScope()->getFunction() == inFunc)
        return {found, false};
    } else {
      auto *global = llvh::cast<GlobalObjectProperty>(found);
      if (inFunc->isGlobalScope()) {
        if (global->isDeclared())
          return {found, false};
        // If it was an ambient property, we need to declare it now and
        // treat it as a new property.
        global->orDeclared(true);
        return {found, true};
      }
    }
  }

  // Create a property if global scope, variable otherwise.
  if (inFunc->isGlobalScope() && declKind == VarDecl::Kind::Var) {
    GlobalObjectProperty *p = Builder.createGlobalObjectProperty(name, true);
    // Register the variable in the scoped hash table.
    nameTable_.insert(name, p);
    return {p, true};
  } else {
    ScopeVar *sv = newLocalVar(declKind, name);
    return {sv, true};
  }
}

ScopeVar *ESTreeIRGen::newLocalVar(VarDecl::Kind declKind, Identifier name) {
  // Check for re-declaration in the same scope.
  if (auto *found = llvh::dyn_cast_or_null<ScopeVar>(nameTable_.lookup(name))) {
    if (found->getScope() == currentIRScopeDesc_) {
      // Both must be "var".
      if (declKind == VarDecl::Kind::Var &&
          found->getDeclKind() == ScopeVar::DeclKind::Var) {
        // Already defined.
        return found;
      }
      // This should not happen, but since we half-support "let", just ignore
      // it.
    }
  }

  ScopeVar::DeclKind svdk;
  if (declKind == VarDecl::Kind::Let)
    svdk = ScopeVar::DeclKind::Let;
  else if (declKind == VarDecl::Kind::ConstLet)
    svdk = ScopeVar::DeclKind::ConstLet;
  else if (declKind == VarDecl::Kind::ConstVar)
    svdk = ScopeVar::DeclKind::ConstVar;
  else {
    assert(declKind == VarDecl::Kind::Var);
    svdk = ScopeVar::DeclKind::Var;
  }

  auto *var = currentIRScopeDesc_->createVariable(svdk, name);

  // For "let" and "const" create the related TDZ flag.
  if (ScopeVar::declKindNeedsTDZ(svdk) &&
      Mod->getContext().getCodeGenerationSettings().enableTDZ) {
    var->setObeysTDZ(true);
  }

  // Register the variable in the scoped hash table.
  nameTable_.insert(name, var);

  return var;
}

GlobalObjectProperty *ESTreeIRGen::declareNewAmbientProperty(Identifier name) {
  assert(
      !isObjectScoping() &&
      "ambient properties should not be declared in object scoping mode");
  auto *prop = Builder.createGlobalObjectProperty(name, false);
  topLevelContext->scopeRAII.bindGlobalProperty(name, prop);
  return prop;
}

void ESTreeIRGen::declareAmbientProperty(Identifier name) {
  if (isObjectScoping())
    return;

  // Avoid redefining global properties.
  if (nameTable_.lookup(name))
    return;
  declareNewAmbientProperty(name);
}

namespace {
/// This visitor structs collects declarations within a single closure without
/// descending into child closures.
struct DeclHoisting {
  /// The list of collected identifiers (variables and functions).
  llvh::SmallVector<ESTree::VariableDeclaratorNode *, 8> decls{};

  /// A list of functions that need to be hoisted and materialized before we
  /// can generate the rest of the function.
  llvh::SmallVector<ESTree::FunctionDeclarationNode *, 8> closures;

  explicit DeclHoisting() = default;
  ~DeclHoisting() = default;

  /// Extract the variable name from the nodes that can define new variables.
  /// The nodes that can define a new variable in the scope are:
  /// VariableDeclarator and FunctionDeclaration>
  void collectDecls(ESTree::Node *V) {
    if (auto VD = llvh::dyn_cast<ESTree::VariableDeclaratorNode>(V)) {
      return decls.push_back(VD);
    }

    if (auto FD = llvh::dyn_cast<ESTree::FunctionDeclarationNode>(V)) {
      return closures.push_back(FD);
    }
  }

  bool shouldVisit(ESTree::Node *V) {
    // Collect declared names, even if we don't descend into children nodes.
    collectDecls(V);

    // Do not descend to child closures because the variables they define are
    // not exposed to the outside function.
    if (llvh::isa<ESTree::FunctionDeclarationNode>(V) ||
        llvh::isa<ESTree::FunctionExpressionNode>(V) ||
        llvh::isa<ESTree::ArrowFunctionExpressionNode>(V))
      return false;
    return true;
  }

  void enter(ESTree::Node *V) {}
  void leave(ESTree::Node *V) {}
};

} // anonymous namespace.

void ESTreeIRGen::processDeclarationFile(ESTree::ProgramNode *programNode) {
  auto Program = dyn_cast_or_null<ESTree::ProgramNode>(programNode);
  if (!Program)
    return;

  DeclHoisting DH;
  Program->visit(DH);

  // Create variable declarations for each of the hoisted variables.
  for (auto vd : DH.decls)
    declareAmbientProperty(getNameFieldFromID(vd->_id));
  for (auto fd : DH.closures)
    declareAmbientProperty(getNameFieldFromID(fd->_id));
}

ESTreeIRGen::FindScopeResult ESTreeIRGen::findClosestScope(
    ScopeDesc *desiredDesc,
    bool ignoreDynamic) const {
  assert(desiredDesc && "desired scope descriptor cannot be null");

  Value *startValue = currentIRScope_;
  ScopeDesc *startDesc = currentIRScopeDesc_;

  // Scan the parent scopes until we reach a dynamic scope, the desired scope,
  // or a new function.
  for (;;) {
    if (!ignoreDynamic && startDesc->isDynamicObject())
      return {startDesc, startValue, startDesc};
    if (startDesc == desiredDesc)
      return {nullptr, startValue, startDesc};

    auto *parentDesc = startDesc->getParent();
    // If the scope isn't available, we have to give up. Most likely
    // because it is in a parent function.
    auto it = curFunction()->availableScopes_.find(parentDesc);
    if (it == curFunction()->availableScopes_.end())
      break;
    startValue = it->second;
    startDesc = parentDesc;
  }

  if (ignoreDynamic)
    return {nullptr, startValue, startDesc};

  // We found the closest scope in the current function and saved it in
  // startValue, startDesc. We did not encounter any dynamic scopes.
  // Keep scanning until we reach a dynamic scope or the desired scope.
  assert(!startDesc->isDynamicObject() && startDesc != desiredDesc);

  ScopeDesc *curDesc = startDesc;
  do {
    curDesc = curDesc->getParent();
    if (curDesc->isDynamicObject())
      return {curDesc, startValue, startDesc};
  } while (curDesc != desiredDesc);

  return {nullptr, startValue, startDesc};
}

ResolvedIdentifier ESTreeIRGen::resolveIdentifier(ESTree::IdentifierNode *id) {
  Identifier name = getNameFieldFromID(id);

  LiteralString *strName;

  // Is this potentially a known variable?
  if (auto *var = nameTable_.lookup(name)) {
    if (auto *SV = llvh::dyn_cast<ScopeVar>(var)) {
      auto closest = findClosestScope(SV->getScope());
      return {
          closest.dynamic ? cast<Value>(Builder.getLiteralString(name))
                          : cast<Value>(SV),
          closest.value,
          closest.desc};
    }

    strName = cast<GlobalObjectProperty>(var)->getName();
  } else {
    if (!isObjectScoping() && curFunction()->function->isStrictMode()) {
      // Report a warning in strict mode.
      auto currentFunc = Builder.getInsertionBlock()->getParent();

      Builder.getModule()->getContext().getSourceErrorManager().warning(
          Warning::UndefinedVariable,
          id->getSourceRange(),
          Twine("the variable \"") + name.str() + "\" was not declared in " +
              currentFunc->getDescriptiveDefinitionKindStr() + " \"" +
              currentFunc->getInternalNameStr() + "\"");

      // Declare a new ambient property to prevent further warnings.
      auto *prop = declareNewAmbientProperty(name);
      strName = prop->getName();
    } else {
      strName = Builder.getLiteralString(name);
    }
  }

  // This is not a known ScopeVar. Find the closest dynamic scope.
  auto closest = findClosestScope(bottomMostScopeDesc_);
  // Is the closest dynamic scope the global scope? If so, we should use it.
  if (closest.dynamic == bottomMostScopeDesc_ &&
      bottomMostScopeDesc_->isGlobalScope()) {
    return {strName, Builder.getGlobalObject(), bottomMostScopeDesc_};
  } else {
    return {strName, closest.value, closest.desc};
  }
}

Value *ESTreeIRGen::genMemberExpressionProperty(
    ESTree::MemberExpressionLikeNode *Mem) {
  // If computed is true, the node corresponds to a computed (a[b]) member
  // lookup and '_property' is an Expression. Otherwise, the node
  // corresponds to a static (a.b) member lookup and '_property' is an
  // Identifier.
  // Details of the computed field are available here:
  // https://github.com/estree/estree/blob/master/spec.md#memberexpression

  if (getComputed(Mem)) {
    return genExpression(getProperty(Mem));
  }

  // Arrays and objects may be accessed with integer indices.
  if (auto N = llvh::dyn_cast<ESTree::NumericLiteralNode>(getProperty(Mem))) {
    return Builder.getLiteralNumber(N->_value);
  }

  // ESTree encodes property access as MemberExpression -> Identifier.
  auto Id = cast<ESTree::IdentifierNode>(getProperty(Mem));

  Identifier fieldName = getNameFieldFromID(Id);
  LLVM_DEBUG(
      dbgs() << "Emitting direct label access to field '" << fieldName
             << "'\n");
  return Builder.getLiteralString(fieldName);
}

bool ESTreeIRGen::canCreateLRefWithoutSideEffects(
    hermes::ESTree::Node *target) {
  // Check for an identifier bound to an existing local variable.
  if (auto *iden = llvh::dyn_cast<ESTree::IdentifierNode>(target)) {
    return dyn_cast_or_null<ScopeVar>(
        nameTable_.lookup(getNameFieldFromID(iden)));
  }

  return false;
}

LReference ESTreeIRGen::createLRef(ESTree::Node *node, bool declInit) {
  SMLoc sourceLoc = node->getDebugLoc();
  IRBuilder::ScopedLocationChange slc(Builder, sourceLoc);

  if (llvh::isa<ESTree::EmptyNode>(node)) {
    LLVM_DEBUG(dbgs() << "Creating an LRef for EmptyNode.\n");
    return LReference(LReference::Empty(), this, sourceLoc);
  }

  /// Create lref for member expression (ex: o.f).
  if (auto *ME = llvh::dyn_cast<ESTree::MemberExpressionNode>(node)) {
    LLVM_DEBUG(dbgs() << "Creating an LRef for member expression.\n");
    Value *obj = genExpression(ME->_object);
    Value *prop = genMemberExpressionProperty(ME);
    return LReference(LReference::Member(), this, obj, prop, sourceLoc);
  }

  /// Create lref for identifiers  (ex: a).
  if (auto *iden = llvh::dyn_cast<ESTree::IdentifierNode>(node)) {
    LLVM_DEBUG(dbgs() << "Creating an LRef for identifier.\n");
    LLVM_DEBUG(
        dbgs() << "Looking for identifier \"" << getNameFieldFromID(iden)
               << "\"\n");
    auto var = resolveIdentifier(iden);
    return LReference(
        LReference::VarOrGlobal(), this, declInit, var, sourceLoc);
  }

  /// Create lref for variable decls (ex: var a).
  if (auto *V = llvh::dyn_cast<ESTree::VariableDeclarationNode>(node)) {
    LLVM_DEBUG(dbgs() << "Creating an LRef for variable declaration.\n");

    assert(V->_declarations.size() == 1 && "Malformed variable declaration");
    auto *decl =
        cast<ESTree::VariableDeclaratorNode>(&V->_declarations.front());

    return createLRef(decl->_id, true);
  }

  // Destructuring assignment.
  if (auto *pat = llvh::dyn_cast<ESTree::PatternNode>(node)) {
    return LReference(LReference::Destructuring(), this, declInit, pat);
  }

  Builder.getModule()->getContext().getSourceErrorManager().error(
      node->getSourceRange(), "unsupported assignment target");

  return LReference(LReference::Error(), this, sourceLoc);
}

Value *ESTreeIRGen::genHermesInternalCall(
    StringRef name,
    Value *thisValue,
    ArrayRef<Value *> args) {
  return Builder.createCallInst(
      Builder.createLoadPropertyInst(
          Builder.createTryLoadGlobalPropertyInst("HermesInternal"), name),
      thisValue,
      args);
}

Value *ESTreeIRGen::genBuiltinCall(
    hermes::BuiltinMethod::Enum builtinIndex,
    ArrayRef<Value *> args) {
  return Builder.createCallBuiltinInst(builtinIndex, args);
}

void ESTreeIRGen::emitEnsureObject(Value *value, StringRef message) {
  // TODO: use "thisArg" when builtins get fixed to support it.
  genBuiltinCall(
      BuiltinMethod::HermesBuiltin_ensureObject,
      {value, Builder.getLiteralString(message)});
}

Value *ESTreeIRGen::emitIteratorSymbol() {
  // FIXME: use the builtin value of @@iterator. Symbol could have been
  // overridden.
  return Builder.createLoadPropertyInst(
      Builder.createTryLoadGlobalPropertyInst("Symbol"), "iterator");
}

ESTreeIRGen::IteratorRecordSlow ESTreeIRGen::emitGetIteratorSlow(Value *obj) {
  auto *method = Builder.createLoadPropertyInst(obj, emitIteratorSymbol());
  auto *iterator = Builder.createCallInst(method, obj, {});

  emitEnsureObject(iterator, "iterator is not an object");
  auto *nextMethod = Builder.createLoadPropertyInst(iterator, "next");

  return {iterator, nextMethod};
}

Value *ESTreeIRGen::emitIteratorNextSlow(IteratorRecordSlow iteratorRecord) {
  auto *nextResult = Builder.createCallInst(
      iteratorRecord.nextMethod, iteratorRecord.iterator, {});
  emitEnsureObject(nextResult, "iterator.next() did not return an object");
  return nextResult;
}

Value *ESTreeIRGen::emitIteratorCompleteSlow(Value *iterResult) {
  return Builder.createLoadPropertyInst(iterResult, "done");
}

Value *ESTreeIRGen::emitIteratorValueSlow(Value *iterResult) {
  return Builder.createLoadPropertyInst(iterResult, "value");
}

void ESTreeIRGen::emitIteratorCloseSlow(
    hermes::irgen::ESTreeIRGen::IteratorRecordSlow iteratorRecord,
    bool ignoreInnerException) {
  auto *haveReturn = Builder.createBasicBlock(Builder.getFunction());
  auto *noReturn = Builder.createBasicBlock(Builder.getFunction());

  auto *returnMethod = genBuiltinCall(
      BuiltinMethod::HermesBuiltin_getMethod,
      {iteratorRecord.iterator, Builder.getLiteralString("return")});
  Builder.createCompareBranchInst(
      returnMethod,
      Builder.getLiteralUndefined(),
      BinaryOperatorInst::OpKind::StrictlyEqualKind,
      noReturn,
      haveReturn);

  Builder.setInsertionBlock(haveReturn);
  if (ignoreInnerException) {
    emitTryCatchScaffolding(
        noReturn,
        // emitBody.
        [this, returnMethod, &iteratorRecord]() {
          Builder.createCallInst(returnMethod, iteratorRecord.iterator, {});
        },
        // emitNormalCleanup.
        []() {},
        // emitHandler.
        [this](BasicBlock *nextBlock) {
          // We need to catch the exception, even if we don't used it.
          Builder.createCatchInst();
          Builder.createBranchInst(nextBlock);
        });
  } else {
    auto *innerResult =
        Builder.createCallInst(returnMethod, iteratorRecord.iterator, {});
    emitEnsureObject(innerResult, "iterator.return() did not return an object");
    Builder.createBranchInst(noReturn);
  }

  Builder.setInsertionBlock(noReturn);
}

ESTreeIRGen::IteratorRecord ESTreeIRGen::emitGetIterator(Value *obj) {
  // Each of these will be modified by "next", so we use a stack storage.
  auto *iterStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("iter"));
  auto *sourceOrNext =
      Builder.createAllocStackInst(genAnonymousLabelName("sourceOrNext"));
  Builder.createStoreStackInst(obj, sourceOrNext);
  auto *iter = Builder.createIteratorBeginInst(sourceOrNext);
  Builder.createStoreStackInst(iter, iterStorage);
  return IteratorRecord{iterStorage, sourceOrNext};
}

void ESTreeIRGen::emitDestructuringAssignment(
    bool declInit,
    ESTree::PatternNode *target,
    Value *source) {
  if (auto *APN = llvh::dyn_cast<ESTree::ArrayPatternNode>(target))
    return emitDestructuringArray(declInit, APN, source);
  else if (auto *OPN = llvh::dyn_cast<ESTree::ObjectPatternNode>(target))
    return emitDestructuringObject(declInit, OPN, source);
  else {
    Mod->getContext().getSourceErrorManager().error(
        target->getSourceRange(), "unsupported destructuring target");
  }
}

void ESTreeIRGen::emitDestructuringArray(
    bool declInit,
    ESTree::ArrayPatternNode *targetPat,
    Value *source) {
  const IteratorRecord iteratorRecord = emitGetIterator(source);

  /// iteratorDone = undefined.
  auto *iteratorDone =
      Builder.createAllocStackInst(genAnonymousLabelName("iterDone"));
  Builder.createStoreStackInst(Builder.getLiteralUndefined(), iteratorDone);

  auto *value =
      Builder.createAllocStackInst(genAnonymousLabelName("iterValue"));

  SharedExceptionHandler handler{};
  handler.exc = Builder.createAllocStackInst(genAnonymousLabelName("exc"));
  // All exception handlers branch to this block.
  handler.exceptionBlock = Builder.createBasicBlock(Builder.getFunction());

  bool first = true;
  bool emittedRest = false;
  // The LReference created in the previous iteration of the destructuring
  // loop. We need it because we want to put the previous store and the creation
  // of the next LReference under one try block.
  llvh::Optional<LReference> lref;

  /// If the previous LReference is valid and non-empty, store "value" into
  /// it and reset the LReference.
  auto storePreviousValue = [&lref, &handler, this, value]() {
    if (lref && !lref->isEmpty()) {
      if (lref->canStoreWithoutSideEffects()) {
        lref->emitStore(Builder.createLoadStackInst(value));
      } else {
        // If we can't store without side effects, wrap the store in try/catch.
        emitTryWithSharedHandler(&handler, [this, &lref, value]() {
          lref->emitStore(Builder.createLoadStackInst(value));
        });
      }
      lref.reset();
    }
  };

  for (auto &elem : targetPat->_elements) {
    ESTree::Node *target = &elem;
    ESTree::Node *init = nullptr;

    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(target)) {
      storePreviousValue();
      emitRestElement(declInit, rest, iteratorRecord, iteratorDone, &handler);
      emittedRest = true;
      break;
    }

    // If we have an initializer, unwrap it.
    if (auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(target)) {
      target = assign->_left;
      init = assign->_right;
    }

    // Can we create the new LReference without side effects and avoid a
    // try/catch. The complexity comes from having to check whether the last
    // LReference also can avoid a try/catch or not.
    if (canCreateLRefWithoutSideEffects(target)) {
      // We don't need a try/catch, but last lref might. Just let the routine
      // do the right thing.
      storePreviousValue();
      lref.emplace(createLRef(target, declInit));
    } else {
      // We need a try/catch, but last lref might not. If it doesn't, emit it
      // directly and clear it, so we won't do anything inside our try/catch.
      if (lref && lref->canStoreWithoutSideEffects()) {
        lref->emitStore(Builder.createLoadStackInst(value));
        lref.reset();
      }
      emitTryWithSharedHandler(
          &handler, [this, &lref, value, target, declInit]() {
            // Store the previous value, if we have one.
            if (lref && !lref->isEmpty())
              lref->emitStore(Builder.createLoadStackInst(value));
            lref = createLRef(target, declInit);
          });
    }

    // Pseudocode of the algorithm for a step:
    //
    //   value = undefined;
    //   if (iteratorDone) goto nextBlock
    // notDoneBlock:
    //   stepResult = IteratorNext(iteratorRecord)
    //   stepDone = IteratorComplete(stepResult)
    //   iteratorDone = stepDone
    //   if (stepDone) goto nextBlock
    // newValueBlock:
    //   value = IteratorValue(stepResult)
    // nextBlock:
    //   if (value !== undefined) goto storeBlock    [if initializer present]
    //   value = initializer                         [if initializer present]
    // storeBlock:
    //   lref.emitStaticStore(value)

    auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *newValueBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *nextBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *getDefaultBlock =
        init ? Builder.createBasicBlock(Builder.getFunction()) : nullptr;
    auto *storeBlock =
        init ? Builder.createBasicBlock(Builder.getFunction()) : nullptr;

    Builder.createStoreStackInst(Builder.getLiteralUndefined(), value);

    // In the first iteration we know that "done" is false.
    if (first) {
      first = false;
      Builder.createBranchInst(notDoneBlock);
    } else {
      Builder.createCondBranchInst(
          Builder.createLoadStackInst(iteratorDone), nextBlock, notDoneBlock);
    }

    // notDoneBlock:
    Builder.setInsertionBlock(notDoneBlock);
    auto *stepValue = emitIteratorNext(iteratorRecord);
    auto *stepDone = emitIteratorComplete(iteratorRecord);
    Builder.createStoreStackInst(stepDone, iteratorDone);
    Builder.createCondBranchInst(
        stepDone, init ? getDefaultBlock : nextBlock, newValueBlock);

    // newValueBlock:
    Builder.setInsertionBlock(newValueBlock);
    Builder.createStoreStackInst(stepValue, value);
    Builder.createBranchInst(nextBlock);

    // nextBlock:
    Builder.setInsertionBlock(nextBlock);

    // NOTE: we can't use emitOptionalInitializationHere() because we want to
    // be able to jump directly to getDefaultBlock.
    if (init) {
      //    if (value !== undefined) goto storeBlock    [if initializer present]
      //    value = initializer                         [if initializer present]
      //  storeBlock:
      Builder.createCondBranchInst(
          Builder.createBinaryOperatorInst(
              Builder.createLoadStackInst(value),
              Builder.getLiteralUndefined(),
              BinaryOperatorInst::OpKind::StrictlyNotEqualKind),
          storeBlock,
          getDefaultBlock);

      Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(target)
          ? getNameFieldFromID(target)
          : Identifier{};

      // getDefaultBlock:
      Builder.setInsertionBlock(getDefaultBlock);
      Builder.createStoreStackInst(genExpression(init, nameHint), value);
      Builder.createBranchInst(storeBlock);

      // storeBlock:
      Builder.setInsertionBlock(storeBlock);
    }
  }

  storePreviousValue();

  // If in the end the iterator is not done, close it. We only need to do
  // that if we didn't end with a rest element because it would have exhausted
  // the iterator.
  if (!emittedRest) {
    auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());
    Builder.createCondBranchInst(
        Builder.createLoadStackInst(iteratorDone), doneBlock, notDoneBlock);

    Builder.setInsertionBlock(notDoneBlock);
    emitIteratorClose(iteratorRecord, false);
    Builder.createBranchInst(doneBlock);

    Builder.setInsertionBlock(doneBlock);
  }

  // If we emitted at least one try block, generate the exception handler.
  if (handler.emittedTry) {
    IRBuilder::SaveRestore saveRestore{Builder};
    Builder.setInsertionBlock(handler.exceptionBlock);

    auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
    auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());

    Builder.createCondBranchInst(
        Builder.createLoadStackInst(iteratorDone), doneBlock, notDoneBlock);

    Builder.setInsertionBlock(notDoneBlock);
    emitIteratorClose(iteratorRecord, true);
    Builder.createBranchInst(doneBlock);

    Builder.setInsertionBlock(doneBlock);
    Builder.createThrowInst(Builder.createLoadStackInst(handler.exc));
  } else {
    // If we didn't use the exception block, we need to delete it, otherwise
    // it fails IR validation even though it will be never executed.
    handler.exceptionBlock->eraseFromParent();

    // Delete the not needed exception stack allocation. It would be optimized
    // out later, but it is nice to produce cleaner non-optimized IR, if it is
    // easy to do so.
    assert(
        !handler.exc->hasUsers() &&
        "should not have any users if no try/catch was emitted");
    handler.exc->eraseFromParent();
  }
}

void ESTreeIRGen::emitRestElement(
    bool declInit,
    ESTree::RestElementNode *rest,
    hermes::irgen::ESTreeIRGen::IteratorRecord iteratorRecord,
    hermes::AllocStackInst *iteratorDone,
    SharedExceptionHandler *handler) {
  // 13.3.3.8 BindingRestElement:...BindingIdentifier

  auto *notDoneBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *newValueBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *doneBlock = Builder.createBasicBlock(Builder.getFunction());

  llvh::Optional<LReference> lref;
  if (canCreateLRefWithoutSideEffects(rest->_argument)) {
    lref = createLRef(rest->_argument, declInit);
  } else {
    emitTryWithSharedHandler(handler, [this, &lref, rest, declInit]() {
      lref = createLRef(rest->_argument, declInit);
    });
  }

  auto *A = Builder.createAllocArrayInst({}, 0);
  auto *n = Builder.createAllocStackInst(genAnonymousLabelName("n"));

  // n = 0.
  Builder.createStoreStackInst(Builder.getLiteralPositiveZero(), n);

  Builder.createCondBranchInst(
      Builder.createLoadStackInst(iteratorDone), doneBlock, notDoneBlock);

  // notDoneBlock:
  Builder.setInsertionBlock(notDoneBlock);
  auto *stepValue = emitIteratorNext(iteratorRecord);
  auto *stepDone = emitIteratorComplete(iteratorRecord);
  Builder.createStoreStackInst(stepDone, iteratorDone);
  Builder.createCondBranchInst(stepDone, doneBlock, newValueBlock);

  // newValueBlock:
  Builder.setInsertionBlock(newValueBlock);
  auto *nVal = Builder.createLoadStackInst(n);
  nVal->setType(Type::createNumber());
  // A[n] = stepValue;
  // Unfortunately this can throw because our arrays can have limited range.
  // The spec doesn't specify what to do in this case, but the reasonable thing
  // to do is to what we would if this was a for-of loop doing the same thing.
  // See section BindingRestElement:...BindingIdentifier, step f and g:
  // https://www.ecma-international.org/ecma-262/9.0/index.html#sec-destructuring-binding-patterns-runtime-semantics-iteratorbindinginitialization
  emitTryWithSharedHandler(handler, [this, stepValue, A, nVal]() {
    Builder.createStorePropertyInst(stepValue, A, nVal);
  });
  // ++n;
  auto add = Builder.createBinaryOperatorInst(
      nVal, Builder.getLiteralNumber(1), BinaryOperatorInst::OpKind::AddKind);
  add->setType(Type::createNumber());
  Builder.createStoreStackInst(add, n);
  Builder.createBranchInst(notDoneBlock);

  // doneBlock:
  Builder.setInsertionBlock(doneBlock);
  if (lref->canStoreWithoutSideEffects()) {
    lref->emitStore(A);
  } else {
    emitTryWithSharedHandler(handler, [&lref, A]() { lref->emitStore(A); });
  }
}

void ESTreeIRGen::emitDestructuringObject(
    bool declInit,
    ESTree::ObjectPatternNode *target,
    Value *source) {
  // Keep track of which keys have been destructured.
  llvh::SmallVector<Value *, 4> excludedItems{};

  if (target->_properties.empty() ||
      llvh::isa<ESTree::RestElementNode>(target->_properties.front())) {
    // ES10.0 13.3.3.5
    // 1. Perform ? RequireObjectCoercible(value).

    // The extremely unlikely case that the user is attempting to destructure
    // into {} or {...rest}. Any other object destructuring will fail upon
    // attempting to retrieve a real property from `source`.
    // We must check that the source can be destructured,
    // and the only time this will throw is if source is undefined or null.
    auto *throwBB = Builder.createBasicBlock(Builder.getFunction());
    auto *doneBB = Builder.createBasicBlock(Builder.getFunction());

    // Use == instead of === to account for both undefined and null.
    Builder.createCondBranchInst(
        Builder.createBinaryOperatorInst(
            source,
            Builder.getLiteralNull(),
            BinaryOperatorInst::OpKind::EqualKind),
        throwBB,
        doneBB);

    Builder.setInsertionBlock(throwBB);
    genBuiltinCall(
        BuiltinMethod::HermesBuiltin_throwTypeError,
        {source,
         Builder.getLiteralString(
             "Cannot destructure 'undefined' or 'null'.")});
    // throwTypeError will always throw.
    // This return is here to ensure well-formed IR, and will not run.
    Builder.createReturnInst(Builder.getLiteralUndefined());

    Builder.setInsertionBlock(doneBB);
  }

  for (auto &elem : target->_properties) {
    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(&elem)) {
      emitRestProperty(declInit, rest, excludedItems, source);
      break;
    }
    auto *propNode = cast<ESTree::PropertyNode>(&elem);

    ESTree::Node *valueNode = propNode->_value;
    ESTree::Node *init = nullptr;

    // If we have an initializer, unwrap it.
    if (auto *assign =
            llvh::dyn_cast<ESTree::AssignmentPatternNode>(valueNode)) {
      valueNode = assign->_left;
      init = assign->_right;
    }

    Identifier nameHint = llvh::isa<ESTree::IdentifierNode>(valueNode)
        ? getNameFieldFromID(valueNode)
        : Identifier{};

    if (llvh::isa<ESTree::IdentifierNode>(propNode->_key) &&
        !propNode->_computed) {
      Identifier key = getNameFieldFromID(propNode->_key);
      excludedItems.push_back(Builder.getLiteralString(key));
      auto *loadedValue = Builder.createLoadPropertyInst(source, key);
      createLRef(valueNode, declInit)
          .emitStore(emitOptionalInitialization(loadedValue, init, nameHint));
    } else {
      Value *key = genExpression(propNode->_key);
      excludedItems.push_back(key);
      auto *loadedValue = Builder.createLoadPropertyInst(source, key);
      createLRef(valueNode, declInit)
          .emitStore(emitOptionalInitialization(loadedValue, init, nameHint));
    }
  }
}

void ESTreeIRGen::emitRestProperty(
    bool declInit,
    ESTree::RestElementNode *rest,
    const llvh::SmallVectorImpl<Value *> &excludedItems,
    hermes::Value *source) {
  auto lref = createLRef(rest->_argument, declInit);

  // Construct the excluded items.
  HBCAllocObjectFromBufferInst::ObjectPropertyMap exMap{};
  llvh::SmallVector<Value *, 4> computedExcludedItems{};
  // Keys need de-duping so we don't create a dummy exclusion object with
  // duplicate keys.
  llvh::DenseSet<Literal *> keyDeDupeSet;
  auto *zeroValue = Builder.getLiteralPositiveZero();

  for (Value *key : excludedItems) {
    if (auto *lit = llvh::dyn_cast<Literal>(key)) {
      // If the key is a literal, we can place it in the
      // HBCAllocObjectFromBufferInst buffer.
      if (keyDeDupeSet.insert(lit).second) {
        exMap.emplace_back(std::make_pair(lit, zeroValue));
      }
    } else {
      // If the key is not a literal, then we have to dynamically populate the
      // excluded object with it after creation from the buffer.
      computedExcludedItems.push_back(key);
    }
  }

  Value *excludedObj;
  if (excludedItems.empty()) {
    excludedObj = Builder.getLiteralUndefined();
  } else {
    // This size is only a hint as the true size may change if there are
    // duplicates when computedExcludedItems is processed at run-time.
    auto excludedSizeHint = exMap.size() + computedExcludedItems.size();
    if (exMap.empty()) {
      excludedObj = Builder.createAllocObjectInst(excludedSizeHint);
    } else {
      excludedObj =
          Builder.createHBCAllocObjectFromBufferInst(exMap, excludedSizeHint);
    }
    for (Value *key : computedExcludedItems) {
      Builder.createStorePropertyInst(zeroValue, excludedObj, key);
    }
  }

  auto *restValue = genBuiltinCall(
      BuiltinMethod::HermesBuiltin_copyDataProperties,
      {Builder.createAllocObjectInst(0), source, excludedObj});

  lref.emitStore(restValue);
}

Value *ESTreeIRGen::emitOptionalInitialization(
    Value *value,
    ESTree::Node *init,
    Identifier nameHint) {
  if (!init)
    return value;

  auto *currentBlock = Builder.getInsertionBlock();
  auto *getDefaultBlock = Builder.createBasicBlock(Builder.getFunction());
  auto *storeBlock = Builder.createBasicBlock(Builder.getFunction());

  //    if (value !== undefined) goto storeBlock    [if initializer present]
  //    value = initializer                         [if initializer present]
  //  storeBlock:
  Builder.createCondBranchInst(
      Builder.createBinaryOperatorInst(
          value,
          Builder.getLiteralUndefined(),
          BinaryOperatorInst::OpKind::StrictlyNotEqualKind),
      storeBlock,
      getDefaultBlock);

  // getDefaultBlock:
  Builder.setInsertionBlock(getDefaultBlock);
  auto *defaultValue = genExpression(init, nameHint);
  auto *defaultResultBlock = Builder.getInsertionBlock();
  Builder.createBranchInst(storeBlock);

  // storeBlock:
  Builder.setInsertionBlock(storeBlock);
  return Builder.createPhiInst(
      {value, defaultValue}, {currentBlock, defaultResultBlock});
}

void ESTreeIRGen::emitNewScope(bool dynamicObjectScope, Value *withValue) {
  Value *parentScope;

  assert(currentIRScopeDesc_ && "currentIRScopeDesc_ can't be null");

  if (currentIRScopeDesc_->getFunction() != curFunction()->function) {
    // If the parent is in a different function, we need to obtain it.
    parentScope = Builder.createGetFunctionParentScopeInst(currentIRScopeDesc_);
    curFunction()->addAvailableScope(currentIRScopeDesc_, parentScope);
  } else {
    parentScope = currentIRScope_;
  }

  ScopeDesc::Kind scopeKind;
  if (dynamicObjectScope) {
    scopeKind = withValue ? ScopeDesc::Kind::With : ScopeDesc::Kind::Eval;
  } else if (isObjectScoping()) {
    scopeKind = ScopeDesc::Kind::StaticObject;
  } else {
    scopeKind = ScopeDesc::Kind::Env;
  }

  ScopeDesc *newDesc =
      curFunction()->function->createScopeDesc(currentIRScopeDesc_, scopeKind);

  Value *newIR;
  if (scopeKind == ScopeDesc::Kind::With) {
    newIR = Builder.createCreateDynamicObjectScopeInst(
        parentScope, newDesc, withValue);
  } else {
    newIR = Builder.createCreateScopeInst(parentScope, newDesc);
  }

  setNewScope(newDesc, newIR);
  curFunction()->addAvailableScope(newDesc, newIR);
}

Instruction *ESTreeIRGen::emitLoad(
    const ResolvedIdentifier &from,
    bool inhibitThrow) {
  if (from.isScopeVar()) {
    ScopeVar *SV = from.getScopeVar();
    Instruction *res;
    res = Builder.createLoadVariableInst(
        SV, from.closestScopeValue, from.closestScopeDesc);
    if (SV->getObeysTDZ())
      res = Builder.createThrowIfEmptyInst(res);
    return res;
  }

  LiteralString *LS = from.getDynamic();

  // If loading from the global scope, use a property instruction.
  if (from.isGlobalProperty()) {
    if (inhibitThrow)
      return Builder.createLoadPropertyInst(Builder.getGlobalObject(), LS);
    else
      return Builder.createTryLoadGlobalPropertyInst(LS);
  }

  // Dynamic load.
  return Builder.createLoadDynamicInst(
      !inhibitThrow, LS, from.closestScopeValue, from.closestScopeDesc);
}

Instruction *ESTreeIRGen::emitStaticLoad(Value *from, bool inhibitThrow) {
  if (isa<ScopeVar>(from)) {
    auto *SV = cast<ScopeVar>(from);
    auto closest = findClosestScope(SV->getScope(), true);
    return emitLoad(
        ResolvedIdentifier(SV, closest.value, closest.desc), inhibitThrow);
  }

  auto *LS = cast<GlobalObjectProperty>(from)->getName();
  if (inhibitThrow)
    return Builder.createLoadPropertyInst(Builder.getGlobalObject(), LS);
  else
    return Builder.createTryLoadGlobalPropertyInst(LS);
}

void ESTreeIRGen::emitStore(
    Value *storedValue,
    const ResolvedIdentifier &ptr,
    bool declInit) {
  if (ptr.isScopeVar()) {
    ScopeVar *SV = ptr.getScopeVar();
    if (declInit) {
      Builder.createStoreVariableInst(
          storedValue, SV, ptr.closestScopeValue, ptr.closestScopeDesc);
      // If a const is initialized. mark it as read-only.
      if (SV->isReadOnly() && isObjectScoping()) {
        Builder.createReadOnlyVariableInst(
            SV->getDeclKind() == ScopeVar::DeclKind::ConstLet,
            SV,
            ptr.closestScopeValue,
            ptr.closestScopeDesc);
      }
      return;
    }

    if (SV->getObeysTDZ()) {
      // Must verify whether the variable is initialized.
      Builder.createThrowIfEmptyInst(Builder.createLoadVariableInst(
          SV, ptr.closestScopeValue, ptr.closestScopeDesc));
    }
    if (SV->isReadOnly()) {
      if (Builder.getFunction()->isStrictMode() ||
          SV->getDeclKind() == ScopeVar::DeclKind::ConstLet) {
        emitRuntimeError(
            Builder, "TypeError", "Assignment to constant variable");
      }
      return;
    }
    Builder.createStoreVariableInst(
        storedValue, SV, ptr.closestScopeValue, ptr.closestScopeDesc);
    return;
  }

  LiteralString *LS = ptr.getDynamic();

  // If storing to the global scope, use a property instruction.
  if (ptr.isGlobalProperty()) {
    if (!Builder.getFunction()->isStrictMode()) {
      Builder.createStorePropertyInst(
          storedValue, Builder.getGlobalObject(), LS);
    } else {
      Builder.createTryStoreGlobalPropertyInst(storedValue, LS);
    }
    return;
  }

  Builder.createStoreDynamicInst(
      Builder.getFunction()->isStrictMode(),
      storedValue,
      LS,
      ptr.closestScopeValue,
      ptr.closestScopeDesc);
}

void ESTreeIRGen::emitStaticStore(
    Value *storedValue,
    Value *ptr,
    bool declInit) {
  if (isa<ScopeVar>(ptr)) {
    auto *SV = cast<ScopeVar>(ptr);
    auto closest = findClosestScope(SV->getScope(), true);
    return emitStore(
        storedValue,
        ResolvedIdentifier(SV, closest.value, closest.desc),
        declInit);
  }

  auto *LS = cast<GlobalObjectProperty>(ptr)->getName();
  if (!Builder.getFunction()->isStrictMode()) {
    Builder.createStorePropertyInst(storedValue, Builder.getGlobalObject(), LS);
  } else {
    Builder.createTryStoreGlobalPropertyInst(storedValue, LS);
  }
}

void ESTreeIRGen::emitRuntimeError(
    IRBuilder &builder,
    StringRef errorType,
    StringRef errorMessage) {
  auto messageLit = builder.getLiteralString(errorMessage);
  if (errorType == "TypeError") {
    builder.createCallBuiltinInst(
        BuiltinMethod::HermesBuiltin_throwTypeError, {messageLit});
  } else {
    // FIXME: must use an instruction for this as the globals could have been
    // overridden.
    builder.createThrowInst(builder.createCallInst(
        builder.createTryLoadGlobalPropertyInst(
            builder.createGlobalObjectProperty(errorType, false)),
        builder.getLiteralUndefined(),
        messageLit));
    // "throw" is a terminator, so we need to start a new basic block.
    builder.setInsertionBlock(builder.createBasicBlock(builder.getFunction()));
  }
}

std::shared_ptr<SerializedScope> ESTreeIRGen::resolveScopeIdentifiers(
    const ScopeChain &chain) {
  std::shared_ptr<SerializedScope> current{};
  for (auto it = chain.functions.rbegin(), end = chain.functions.rend();
       it < end;
       it++) {
    auto next = std::make_shared<SerializedScope>();
    next->variables.reserve(it->variables.size());
    for (auto var : it->variables) {
      next->variables.push_back(std::move(Builder.createIdentifier(var)));
    }
    next->parentScope = current;
    current = next;
  }
  return current;
}

void ESTreeIRGen::materializeScopesInChain(
    Function *wrapperFunction,
    const std::shared_ptr<const SerializedScope> &scope,
    int depth) {
  if (!scope)
    return;
  assert(depth < 1000 && "Excessive scope depth");

  // First materialize parent scopes.
  materializeScopesInChain(wrapperFunction, scope->parentScope, depth - 1);

  // If scope->closureAlias is specified, we must create an alias binding
  // between originalName (which must be valid) and the variable identified by
  // closureAlias.
  //
  // We do this *before* inserting the other variables below to reflect that
  // the closure alias is conceptually in an outside scope and also avoid the
  // closure name incorrectly shadowing the same name inside the closure.
  if (scope->closureAlias.isValid()) {
    assert(scope->originalName.isValid() && "Original name invalid");
    assert(
        scope->originalName != scope->closureAlias &&
        "Original name must be different from the alias");

    // NOTE: the closureAlias target must exist and must be a Variable.
    auto *closureVar = cast<ScopeVar>(nameTable_.lookup(scope->closureAlias));

    // Re-create the alias.
    nameTable_.insert(scope->originalName, closureVar);
  }

  // Create an external scope.
  ExternalScope *ES = Builder.createExternalScope(wrapperFunction, depth);
  for (auto variableId : scope->variables) {
    auto *variable =
        Builder.createVariable(ES, Variable::DeclKind::Var, variableId);
    nameTable_.insert(variableId, variable);
  }
}

/// Scope analysis works through tracking CreateFunctionInst, so we need to
/// create dummy parents.
/// This function is not actually used, since lazy compilation and debugging
/// are disabled.
static std::pair<Function *, ScopeDesc *> createDummyLexicalParents(
    IRBuilder &builder,
    Function *global,
    const std::shared_ptr<const SerializedScope> &scope,
    Identifier childName,
    Function *childFunc) {
  Function *ourFunc;
  ScopeDesc *parentDesc;
  ScopeDesc *ourDesc;

  if (!childFunc) {
    childFunc = builder.createFunction(
        childName, Function::DefinitionKind::ES5Function, false, {}, false);
  }

  if (!scope || !scope->parentScope) {
    ourFunc = global;
    parentDesc = nullptr;
  } else {
    std::tie(ourFunc, parentDesc) = createDummyLexicalParents(
        builder, global, scope->parentScope, scope->originalName, nullptr);
  }

  auto *block = builder.createBasicBlock(ourFunc);
  builder.setInsertionBlock(block);
  builder.createUnreachableInst();
  ourDesc = global->createScopeDesc(parentDesc, ScopeDesc::Kind::Env);
  auto *scopeVal = builder.createCreateScopeInst(nullptr, ourDesc);
  auto *inst = builder.createCreateFunctionInst(childFunc, scopeVal, ourDesc);
  builder.createReturnInst(inst);

  if (scope) {
    for (auto var : scope->variables) {
      ourDesc->createVariable(ScopeVar::DeclKind::Var, var);
    }
  }

  return {childFunc, ourDesc};
}

/// Add dummy functions for lexical scope debug info.
// They are never executed and serve no purpose other than filling in debug
// info. This is currently necessary because we can't rely on parent bytecode
// modules for lexical scoping data.
void ESTreeIRGen::addLexicalDebugInfo(
    Function *child,
    Function *global,
    const std::shared_ptr<const SerializedScope> &scope) {
  createDummyLexicalParents(Builder, global, scope, Identifier{}, child);
}

std::shared_ptr<SerializedScope> ESTreeIRGen::serializeScope(
    FunctionContext *ctx,
    bool includeGlobal) {
  // Serialize the global scope if and only if it's the only scope.
  // We serialize the global scope to avoid re-declaring variables,
  // and only do it once to avoid creating spurious scopes.
  if (!ctx || (ctx->function->isGlobalScope() && !includeGlobal))
    return lexicalScopeChain;

  auto scope = std::make_shared<SerializedScope>();
  auto *func = ctx->function;
  assert(func && "Missing function when saving scope");

  scope->originalName = func->getOriginalOrInferredName();
  if (auto *closure = func->getLazyClosureAlias()) {
    scope->closureAlias = closure->getName();
  }
  for (auto *var : func->getFunctionScope()->getVariables()) {
    scope->variables.push_back(var->getName());
  }
  scope->parentScope = serializeScope(ctx->getPreviousContext(), false);
  return scope;
}

} // namespace irgen
} // namespace hermes
