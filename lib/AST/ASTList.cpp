/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/ASTList.h"

#include "hermes/AST/Context.h"

namespace hermes {
namespace ESTree {

void ASTList::push_back(Context &ctx, hermes::ESTree::Node *node) {
  Chunk *chunk;
  if (LLVM_UNLIKELY(!first_)) {
    // Allocate capacity for one extra element.
    first_ = chunk = allocateChunk(ctx, INITIAL_CAPACITY + 1);
    // Keep the logical capacity to INITIAL_CAPACITY.
    chunk->capacity = INITIAL_CAPACITY;
    setLastChunk(chunk);
  } else {
    chunk = getLastChunk();
    if (LLVM_UNLIKELY(chunk->size == chunk->capacity)) {
      uint_fast16_t newCapacity = chunk->capacity;
      if (newCapacity != MAX_CAPACITY)
        newCapacity *= 2;
      auto *newChunk = allocateChunk(ctx, newCapacity);
      chunk->next = newChunk;
      setLastChunk(newChunk);
      chunk = newChunk;
    }
  }

  assert(
      chunk->size < chunk->capacity &&
      "chunk size must have been already checked");
  chunk->data[chunk->size++] = node;
}

ASTList::Chunk *ASTList::allocateChunk(Context &ctx, uint_fast16_t capacity) {
  auto *chunk = static_cast<Chunk *>(
      ctx.allocateNode(chunkSize(capacity), alignof(Chunk)));
  chunk->next = nullptr;
  chunk->size = 0;
  chunk->capacity = capacity;
  return chunk;
}

} // namespace ESTree
} // namespace hermes
