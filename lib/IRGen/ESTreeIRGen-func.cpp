/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ESTreeIRGen.h"

#include "llvh/ADT/SmallString.h"

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// FunctionContext

FunctionContext::FunctionContext(
    ESTreeIRGen *irGen,
    Function *function,
    sem::FunctionInfo *semInfo)
    : irGen_(irGen),
      semInfo_(semInfo),
      oldContext_(irGen->functionContext_),
      builderSaveState_(irGen->Builder),
      function(function),
      scopeRAII(irGen) {
  irGen->functionContext_ = this;

  // Initialize it to LiteraUndefined by default to avoid corner cases.
  this->capturedNewTarget = irGen->Builder.getLiteralUndefined();

  if (semInfo_) {
    // Allocate the label table. Each label definition will be encountered in
    // the AST before it is referenced (because of the nature of JavaScript), at
    // which point we will initialize the GotoLabel structure with basic blocks
    // targets.
    labels_.resize(semInfo_->labelCount);
  }
}

FunctionContext::~FunctionContext() {
  irGen_->functionContext_ = oldContext_;
}

Identifier FunctionContext::genAnonymousLabelName(StringRef hint) {
  llvh::SmallString<16> buf;
  llvh::raw_svector_ostream nameBuilder{buf};
  nameBuilder << "?anon_" << anonymousLabelCounter++ << "_" << hint;
  return function->getContext().getIdentifier(nameBuilder.str());
}

//===----------------------------------------------------------------------===//
// ESTreeIRGen

void ESTreeIRGen::genFunctionDeclaration(
    ESTree::FunctionDeclarationNode *func) {
  if (func->_async && func->_generator) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        func->getSourceRange(), Twine("async generators are unsupported"));
    return;
  }

  // Find the name of the function.
  Identifier functionName = getNameFieldFromID(func->_id);
  LLVM_DEBUG(dbgs() << "IRGen function \"" << functionName << "\".\n");

  auto *funcStorage = nameTable_.lookup(functionName);
  assert(
      funcStorage && "function declaration variable should have been hoisted");

  Function *newFunc = func->_async
      ? genAsyncFunction(functionName, nullptr, func)
      : func->_generator ? genGeneratorFunction(functionName, nullptr, func)
                         : genES5Function(functionName, nullptr, func);

  // Store the newly created closure into a frame variable with the same name.
  auto *newClosure = Builder.createCreateFunctionInst(
      newFunc, currentIRScope_, currentIRScopeDesc_);

  emitStore(newClosure, funcStorage, true);
}

Value *ESTreeIRGen::genFunctionExpression(
    ESTree::FunctionExpressionNode *FE,
    Identifier nameHint) {
  if (FE->_async && FE->_generator) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        FE->getSourceRange(), Twine("async generators are unsupported"));
    return Builder.getLiteralUndefined();
  }

  LLVM_DEBUG(
      dbgs() << "Creating anonymous closure. "
             << Builder.getInsertionBlock()->getParent()->getInternalName()
             << ".\n");

  LexicalScopeRAII newScope(this);
  ScopeVar *nameVar = nullptr;

  Identifier originalNameIden = nameHint;
  // If the function expression has a name, we must bind it in a new scope.
  if (FE->_id) {
    originalNameIden = getNameFieldFromID(FE->_id);
    emitNewScope();
    nameVar = newLocalVar(VarDecl::Kind::ConstVar, originalNameIden);
  }

  Function *newFunc = FE->_async
      ? genAsyncFunction(originalNameIden, nameVar, FE)
      : FE->_generator ? genGeneratorFunction(originalNameIden, nameVar, FE)
                       : genES5Function(originalNameIden, nameVar, FE);

  Value *closure = Builder.createCreateFunctionInst(
      newFunc, currentIRScope_, currentIRScopeDesc_);

  if (nameVar)
    emitStore(closure, nameVar, true);

  return closure;
}

Value *ESTreeIRGen::genArrowFunctionExpression(
    ESTree::ArrowFunctionExpressionNode *AF,
    Identifier nameHint) {
  LLVM_DEBUG(
      dbgs() << "Creating arrow function. "
             << Builder.getInsertionBlock()->getParent()->getInternalName()
             << ".\n");

  if (AF->_async) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        AF->getSourceRange(), Twine("async functions are unsupported"));
    return Builder.getLiteralUndefined();
  }

  auto *newFunc = Builder.createFunction(
      nameHint,
      Function::DefinitionKind::ES6Arrow,
      ESTree::isStrict(AF->strictness),
      AF->getSourceRange());

  {
    FunctionContext newFunctionContext{this, newFunc, AF->getSemInfo()};

    // Propagate captured "this", "new.target" and "arguments" from parents.
    auto *prev = curFunction()->getPreviousContext();
    curFunction()->capturedThis = prev->capturedThis;
    curFunction()->capturedNewTarget = prev->capturedNewTarget;
    curFunction()->capturedArguments = prev->capturedArguments;

    emitFunctionPrologue(
        AF,
        Builder.createBasicBlock(newFunc),
        InitES5CaptureState::No,
        DoEmitParameters::Yes);

    genStatement(AF->_body);
    emitFunctionEpilogue(Builder.getLiteralUndefined());
  }

  // Emit CreateFunctionInst after we have restored the builder state.
  return Builder.createCreateFunctionInst(
      newFunc, currentIRScope_, currentIRScopeDesc_);
}

#ifndef HERMESVM_LEAN
namespace {
ESTree::NodeKind getLazyFunctionKind(ESTree::FunctionLikeNode *node) {
  if (node->isMethodDefinition) {
    // This is not a regular function expression but getter/setter.
    // If we want to reparse it later, we have to start from an
    // identifier and not from a 'function' keyword.
    return ESTree::NodeKind::Property;
  }
  return node->getKind();
}
} // namespace
Function *ESTreeIRGen::genES5Function(
    Identifier originalName,
    ScopeVar *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode,
    bool isGeneratorInnerFunction) {
  assert(functionNode && "Function AST cannot be null");

  auto *body = ESTree::getBlockStatement(functionNode);
  assert(body && "body of ES5 function cannot be null");

  Function *newFunction = isGeneratorInnerFunction
      ? Builder.createGeneratorInnerFunction(
            originalName,
            Function::DefinitionKind::ES5Function,
            ESTree::isStrict(functionNode->strictness),
            functionNode->getSourceRange(),
            /* insertBefore */ nullptr)
      : Builder.createFunction(
            originalName,
            Function::DefinitionKind::ES5Function,
            ESTree::isStrict(functionNode->strictness),
            functionNode->getSourceRange(),
            /* isGlobal */ false,
            /* insertBefore */ nullptr);

  newFunction->setLazyClosureAlias(lazyClosureAlias);

  if (auto *bodyBlock = llvh::dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      assert(
          !isGeneratorInnerFunction &&
          "generator inner function should be included with outer function");
      setupLazyScope(functionNode, newFunction, body);
      return newFunction;
    }
  }

  FunctionContext newFunctionContext{
      this, newFunction, functionNode->getSemInfo()};

  if (isGeneratorInnerFunction) {
    // StartGeneratorInst
    // ResumeGeneratorInst
    // at the beginning of the function, to allow for the first .next() call.
    auto *initGenBB = Builder.createBasicBlock(newFunction);
    Builder.setInsertionBlock(initGenBB);
    Builder.createStartGeneratorInst();
    auto *prologueBB = Builder.createBasicBlock(newFunction);
    auto *prologueResumeIsReturn = Builder.createAllocStackInst(
        genAnonymousLabelName("isReturn_prologue"));
    genResumeGenerator(GenFinally::No, prologueResumeIsReturn, prologueBB);

    if (hasSimpleParams(functionNode)) {
      // If there are simple params, then we don't need an extra yield/resume.
      // They can simply be initialized on the first call to `.next`.
      Builder.setInsertionBlock(prologueBB);
      emitFunctionPrologue(
          functionNode,
          prologueBB,
          InitES5CaptureState::Yes,
          DoEmitParameters::Yes);
    } else {
      // If there are non-simple params, then we must add a new yield/resume.
      // The `.next()` call will occur once in the outer function, before
      // the iterator is returned to the caller of the `function*`.
      auto *entryPointBB = Builder.createBasicBlock(newFunction);
      auto *entryPointResumeIsReturn =
          Builder.createAllocStackInst(genAnonymousLabelName("isReturn_entry"));

      // Initialize parameters.
      Builder.setInsertionBlock(prologueBB);
      emitFunctionPrologue(
          functionNode,
          prologueBB,
          InitES5CaptureState::Yes,
          DoEmitParameters::Yes);
      Builder.createSaveAndYieldInst(
          Builder.getLiteralUndefined(), entryPointBB);

      // Actual entry point of function from the caller's perspective.
      Builder.setInsertionBlock(entryPointBB);
      genResumeGenerator(
          GenFinally::No,
          entryPointResumeIsReturn,
          Builder.createBasicBlock(newFunction));
    }
  } else {
    emitFunctionPrologue(
        functionNode,
        Builder.createBasicBlock(newFunction),
        InitES5CaptureState::Yes,
        DoEmitParameters::Yes);
  }

  genStatement(body);
  emitFunctionEpilogue(Builder.getLiteralUndefined());

  return curFunction()->function;
}
#endif

Function *ESTreeIRGen::genGeneratorFunction(
    Identifier originalName,
    ScopeVar *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode) {
  assert(functionNode && "Function AST cannot be null");

  if (!Builder.getModule()->getContext().isGeneratorEnabled()) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        functionNode->getSourceRange(), "generator compilation is disabled");
  }

  // Build the outer function which creates the generator.
  // Does not have an associated source range.
  auto *outerFn = Builder.createGeneratorFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      functionNode->getSourceRange(),
      /* insertBefore */ nullptr);
  outerFn->setLazyClosureAlias(lazyClosureAlias);

  auto *body = ESTree::getBlockStatement(functionNode);
  if (auto *bodyBlock = llvh::dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      setupLazyScope(functionNode, outerFn, body);
      return outerFn;
    }
  }

  {
    FunctionContext outerFnContext{this, outerFn, functionNode->getSemInfo()};

    emitFunctionPrologue(
        functionNode,
        Builder.createBasicBlock(outerFn),
        InitES5CaptureState::Yes,
        DoEmitParameters::No);

    // Build the inner function. This must be done in the outerFnContext
    // since it's lexically considered a child function.
    auto *innerFn = genES5Function(
        genAnonymousLabelName(originalName.isValid() ? originalName.str() : ""),
        nullptr,
        functionNode,
        true);

    // Create a generator function, which will store the arguments.
    auto *gen = Builder.createCreateGeneratorInst(
        innerFn, currentIRScope_, currentIRScopeDesc_);

    if (!hasSimpleParams(functionNode)) {
      // If there are non-simple params, step the inner function once to
      // initialize them.
      Value *next = Builder.createLoadPropertyInst(gen, "next");
      Builder.createCallInst(next, gen, {});
    }

    emitFunctionEpilogue(gen);
  }

  return outerFn;
}

void ESTreeIRGen::setupLazyScope(
    ESTree::FunctionLikeNode *functionNode,
    Function *function,
    ESTree::BlockStatementNode *bodyBlock) {
  assert(
      bodyBlock->isLazyFunctionBody &&
      "setupLazyScope can only be used with lazy function bodies");
  // Set the AST position and variable context so we can continue later.
  function->setLazyScope(saveCurrentScope());
  auto &lazySource = function->getLazySource();
  lazySource.bufferId = bodyBlock->bufferId;
  lazySource.nodeKind = getLazyFunctionKind(functionNode);
  lazySource.functionRange = functionNode->getSourceRange();
  lazySource.paramYield = bodyBlock->paramYield;
  lazySource.paramAwait = bodyBlock->paramAwait;

  // Set the function's .length.
  function->setExpectedParamCountIncludingThis(
      countExpectedArgumentsIncludingThis(functionNode));
}

Function *ESTreeIRGen::genAsyncFunction(
    Identifier originalName,
    ScopeVar *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode) {
  assert(functionNode && "Function AST cannot be null");

  if (!Builder.getModule()->getContext().isGeneratorEnabled()) {
    Builder.getModule()->getContext().getSourceErrorManager().error(
        functionNode->getSourceRange(),
        "async function compilation requires enabling generator");
  }

  auto *asyncFn = Builder.createAsyncFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      functionNode->getSourceRange(),
      /* insertBefore */ nullptr);

  // Setup lazy compilation
  asyncFn->setLazyClosureAlias(lazyClosureAlias);

  auto *body = ESTree::getBlockStatement(functionNode);
  if (auto *bodyBlock = llvh::dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      setupLazyScope(functionNode, asyncFn, body);
      return asyncFn;
    }
  }

  {
    FunctionContext asyncFnContext{this, asyncFn, functionNode->getSemInfo()};

    // The outer async function need not emit code for parameters.
    // It would simply delegate `arguments` object down to inner generator.
    // This avoid emitting code e.g. destructuring parameters twice.
    emitFunctionPrologue(
        functionNode,
        Builder.createBasicBlock(asyncFn),
        InitES5CaptureState::Yes,
        DoEmitParameters::No);

    // Build the inner generator. This must be done in the outerFnContext
    // since it's lexically considered a child function.
    auto *gen = genGeneratorFunction(
        genAnonymousLabelName(originalName.isValid() ? originalName.str() : ""),
        lazyClosureAlias,
        functionNode);

    auto *genClosure = Builder.createCreateFunctionInst(
        gen, currentIRScope_, currentIRScopeDesc_);
    auto *thisArg = curFunction()->function->getThisParameter();
    auto *argumentsList = curFunction()->createArgumentsInst;

    auto *spawnAsyncClosure = Builder.createGetBuiltinClosureInst(
        BuiltinMethod::HermesBuiltin_spawnAsync);

    auto *res = Builder.createCallInst(
        spawnAsyncClosure,
        Builder.getLiteralUndefined(),
        {genClosure, thisArg, argumentsList});

    emitFunctionEpilogue(res);
  }
  return asyncFn;
}

void ESTreeIRGen::initCaptureStateInES5FunctionHelper() {
  // Capture "this", "new.target" and "arguments" if there are inner arrows.
  if (!curFunction()->getSemInfo()->containsArrowFunctions)
    return;

  // "this".
  curFunction()->capturedThis =
      newLocalVar(VarDecl::Kind::Var, genAnonymousLabelName("this"));
  emitStore(
      Builder.getFunction()->getThisParameter(),
      curFunction()->capturedThis,
      true);

  // "new.target".
  curFunction()->capturedNewTarget =
      newLocalVar(VarDecl::Kind::Var, genAnonymousLabelName("new.target"));
  emitStore(
      Builder.createGetNewTargetInst(), curFunction()->capturedNewTarget, true);

  // "arguments".
  if (curFunction()->getSemInfo()->containsArrowFunctionsUsingArguments) {
    curFunction()->capturedArguments =
        newLocalVar(VarDecl::Kind::Var, genAnonymousLabelName("arguments"));
    emitStore(
        curFunction()->createArgumentsInst,
        curFunction()->capturedArguments,
        true);
  }
}

void ESTreeIRGen::emitFunctionPrologue(
    ESTree::FunctionLikeNode *funcNode,
    BasicBlock *entry,
    InitES5CaptureState doInitES5CaptureState,
    DoEmitParameters doEmitParameters) {
  auto *newFunc = curFunction()->function;
  auto *semInfo = curFunction()->getSemInfo();
  LLVM_DEBUG(
      dbgs() << "Hoisting "
             << (semInfo->varDecls.size() + semInfo->closures.size())
             << " variable decls.\n");

  Builder.setLocation(newFunc->getSourceRange().Start);

  // Start pumping instructions into the entry basic block.
  Builder.setInsertionBlock(entry);

  // Always insert a CreateArgumentsInst. We will delete it later if it is
  // unused.
  curFunction()->createArgumentsInst = Builder.createCreateArgumentsInst();

  // Open a new lexical scope.
  emitNewScope();

  // Create variable declarations for each of the hoisted variables and
  // functions. Initialize only the variables to undefined.
  for (auto decl : semInfo->varDecls) {
    auto res = declareVariableOrGlobalProperty(
        newFunc, decl.kind, getNameFieldFromID(decl.identifier));
    // If already declared, skip.
    if (!res.second)
      continue;

    if (auto *globalProp = llvh::dyn_cast<GlobalObjectProperty>(res.first)) {
      Builder.createDeclareGlobalVarInst(globalProp->getName());
    } else {
      auto *var = llvh::cast<ScopeVar>(res.first);

      // Otherwise, initialize it to undefined or empty, depending on TDZ.
      Builder.createStoreVariableInst(
          var->getObeysTDZ() ? (Literal *)Builder.getLiteralEmpty()
                             : (Literal *)Builder.getLiteralUndefined(),
          var,
          currentIRScope_,
          currentIRScopeDesc_);
    }
  }
  for (auto *fd : semInfo->closures) {
    auto res = declareVariableOrGlobalProperty(
        newFunc, VarDecl::Kind::Var, getNameFieldFromID(fd->_id));
    if (res.second && llvh::isa<GlobalObjectProperty>(res.first)) {
      auto *globalProp = llvh::cast<GlobalObjectProperty>(res.first);
      Builder.createDeclareGlobalVarInst(globalProp->getName());
    }
  }

  // Always create the "this" parameter. It needs to be created before we
  // initialized the ES5 capture state.
  Builder.createParameter(newFunc, "this");

  if (doInitES5CaptureState != InitES5CaptureState::No)
    initCaptureStateInES5FunctionHelper();

  // Construct the parameter list. Create function parameters and register
  // them in the scope.
  if (doEmitParameters == DoEmitParameters::Yes) {
    emitParameters(funcNode);
  } else {
    newFunc->setExpectedParamCountIncludingThis(
        countExpectedArgumentsIncludingThis(funcNode));
  }

  // Generate the code for import declarations before generating the rest of the
  // body.
  for (auto importDecl : semInfo->imports) {
    genImportDeclaration(importDecl);
  }

  // Generate and initialize the code for the hoisted function declarations
  // before generating the rest of the body.
  for (auto funcDecl : semInfo->closures) {
    genFunctionDeclaration(funcDecl);
  }
}

void ESTreeIRGen::emitParameters(ESTree::FunctionLikeNode *funcNode) {
  auto *newFunc = curFunction()->function;

  LLVM_DEBUG(dbgs() << "IRGen function parameters.\n");

  // Create a variable for every parameter.
  for (auto paramDecl : funcNode->getSemInfo()->paramNames) {
    Identifier paramName = getNameFieldFromID(paramDecl.identifier);
    LLVM_DEBUG(dbgs() << "Adding parameter: " << paramName << "\n");
    newLocalVar(VarDecl::Kind::Var, paramName);
  }

  // FIXME: T42569352 TDZ for parameters used in initializer expressions.
  uint32_t paramIndex = uint32_t{0} - 1;
  for (auto &elem : ESTree::getParams(funcNode)) {
    ESTree::Node *param = &elem;
    ESTree::Node *init = nullptr;
    ++paramIndex;

    if (auto *rest = llvh::dyn_cast<ESTree::RestElementNode>(param)) {
      createLRef(rest->_argument, true)
          .emitStore(genBuiltinCall(
              BuiltinMethod::HermesBuiltin_copyRestArgs,
              Builder.getLiteralNumber(paramIndex)));
      break;
    }

    // Unpack the optional initialization.
    if (auto *assign = llvh::dyn_cast<ESTree::AssignmentPatternNode>(param)) {
      param = assign->_left;
      init = assign->_right;
    }

    Identifier formalParamName = llvh::isa<ESTree::IdentifierNode>(param)
        ? getNameFieldFromID(param)
        : genAnonymousLabelName("param");

    auto *formalParam = Builder.createParameter(newFunc, formalParamName);
    createLRef(param, true)
        .emitStore(
            emitOptionalInitialization(formalParam, init, formalParamName));
  }

  newFunc->setExpectedParamCountIncludingThis(
      countExpectedArgumentsIncludingThis(funcNode));
}

uint32_t ESTreeIRGen::countExpectedArgumentsIncludingThis(
    ESTree::FunctionLikeNode *funcNode) {
  // Start at 1 to account for "this".
  uint32_t count = 1;
  for (auto &param : ESTree::getParams(funcNode)) {
    if (llvh::isa<ESTree::AssignmentPatternNode>(param)) {
      // Found an initializer, stop counting expected arguments.
      break;
    }
    ++count;
  }
  return count;
}

void ESTreeIRGen::emitFunctionEpilogue(Value *returnValue) {
  if (returnValue) {
    Builder.setLocation(SourceErrorManager::convertEndToLocation(
        Builder.getFunction()->getSourceRange()));
    Builder.createReturnInst(returnValue);
  }

  // Delete CreateArgumentsInst if it is unused.
  if (!curFunction()->createArgumentsInst->hasUsers())
    curFunction()->createArgumentsInst->eraseFromParent();

  curFunction()->function->clearStatementCount();
}

void ESTreeIRGen::genDummyFunction(Function *dummy) {
  IRBuilder builder{dummy};

  builder.createParameter(dummy, "this");
  BasicBlock *firstBlock = builder.createBasicBlock(dummy);
  builder.setInsertionBlock(firstBlock);
  builder.createUnreachableInst();
  builder.createReturnInst(builder.getLiteralUndefined());
}

/// Generate a function which immediately throws the specified SyntaxError
/// message.
Function *ESTreeIRGen::genSyntaxErrorFunction(
    Module *M,
    Identifier originalName,
    SMRange sourceRange,
    StringRef error) {
  IRBuilder builder{M};

  Function *function = builder.createFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      true,
      sourceRange,
      false);

  builder.createParameter(function, "this");
  BasicBlock *firstBlock = builder.createBasicBlock(function);
  builder.setInsertionBlock(firstBlock);
  emitRuntimeError(builder, "SyntaxError", error);
  return function;
}

} // namespace irgen
} // namespace hermes
