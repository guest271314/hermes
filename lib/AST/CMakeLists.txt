# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_llvm_library(hermesAST
    ASTList.cpp
    ASTBuilder.cpp
    ESTree.cpp
    ESTreeJSONDumper.cpp
    RecursiveVisitor.h
    SemValidate.cpp
    SemanticValidator.cpp SemanticValidator.h
    ValidatorHelpers.cpp ValidatorHelpers.h
    CommonJS.cpp
    LINK_LIBS
    hermesSupport
)
