/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/ASTList.h"
#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"

#include "gtest/gtest.h"
using llvm::cast;
using llvm::dyn_cast;

using namespace hermes;
using namespace hermes::ESTree;

namespace {

TEST(ASTListTest, EmptyTest) {
  Context context;

  ASTList list{};
  ASSERT_TRUE(list.empty());
  ASSERT_TRUE(list.begin() == list.end());

  for (unsigned i = 0; i != 100; ++i) {
    auto *node = new (context) NumericLiteralNode(i);
    list.push_back(context, node);
  }

  unsigned size = 0;
  for (auto *n : list) {
    ASSERT_EQ(size, cast<NumericLiteralNode>(n)->_value);
    ++size;
  }
  ASSERT_EQ(100, size);
}

} // end anonymous namespace
