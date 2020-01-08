/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_AST_ASTLIST_H
#define HERMES_AST_ASTLIST_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hermes {

class Context;

namespace ESTree {

class Node;

/// A simple non-intrusive, non-owning list, designed for bump pointer
/// allocation. It never frees anything, since it assumes that it will be freed
/// in bulk at the end.
/// To save space, it is optimized to occupy just a single null pointer when
/// empty. Only forward iteration is possible.
/// List elements are stored in a singly list of chunks with increasing size.
class ASTList {
  /// Contains some number of list elements. If this is the first chunk, one
  /// extra data element is allocated and used to store the pointer to the last
  /// chunk.
  struct Chunk {
    Chunk *next;
    uint16_t size;
    uint16_t capacity;
    Node *data[];
  };

  /// \return the number of bytes needed for a chunk of capacity \p capacity.
  static size_t chunkSize(uint_fast16_t capacity) {
    return offsetof(Chunk, data) + sizeof(Node *) * capacity;
  }

  /// Capacity of the first chunk.
  static constexpr unsigned INITIAL_CAPACITY = 4;
  /// Capacity of the largest chunk we can allocate. For convenience we keep
  /// it a power of two.
  static constexpr unsigned MAX_CAPACITY = 1 << 15;

 public:
  ASTList() = default;
  ~ASTList() = default;

  ASTList(const ASTList &) = delete;
  void operator=(const ASTList &) = delete;

  ASTList(ASTList &&o) noexcept : first_(o.first_) {
    o.first_ = nullptr;
  }

  ASTList &operator=(ASTList &&o) noexcept {
    first_ = o.first_;
    o.first_ = nullptr;
    return *this;
  }

  void swap(ASTList &o) noexcept {
    std::swap(first_, o.first_);
  }

  /// \return true if the list is empty.
  bool empty() const {
    return first_ == nullptr;
  }

  /// Append a new node to the end of the list.
  void push_back(Context &ctx, Node *node);

  /// List iterator.
  template <bool isConst>
  class IteratorBase {
    using T = Node *;
    using QualifiedT = typename std::conditional<isConst, const T, T>::type;

   public:
    template <bool otherConst>
    bool operator==(const IteratorBase<otherConst> &o) const {
      return chunk_ == o.chunk_ && index_ == o.index_;
    }
    template <bool otherConst>
    bool operator!=(const IteratorBase<otherConst> &o) const {
      return !this->operator==(o);
    }

    IteratorBase &operator++() {
      assert(chunk_ && "incrementing end iterator");
      if (++index_ == chunk_->size) {
        chunk_ = chunk_->next;
        index_ = 0;
      }
      return *this;
    }

    QualifiedT &operator*() const {
      assert(
          chunk_ && index_ < chunk_->size && "dereferencing invalid iterator");
      return chunk_->data[index_];
    }

   private:
    friend class ASTList;

    Chunk *chunk_;
    uint16_t index_;

    IteratorBase(Chunk *chunk) : chunk_(chunk), index_(0) {}
  };

  using iterator = IteratorBase<false>;
  using const_iterator = IteratorBase<true>;

  iterator begin() {
    return first_;
  }
  iterator end() {
    return nullptr;
  }
  const_iterator begin() const {
    return first_;
  }
  const_iterator end() const {
    return nullptr;
  }

 private:
  /// Pointer to the first chunk. Its "extra" data element is used as a pointer
  /// to the last chunk.
  Chunk *first_ = nullptr;

  /// Allocate a new chunk with the specified capacity and initialize its fields
  /// to their default values.
  Chunk *allocateChunk(Context &ctx, uint_fast16_t capacity);

  /// \return the last chunk. This assumes that the list is not empty.
  Chunk *getLastChunk() {
    assert(!empty() && "empty list has no last chunk");
    return (Chunk *)(void *)first_->data[INITIAL_CAPACITY];
  }
  /// Update the pointer to the last chunk of the list, which is stored in the
  /// "extra" data element of the first chunk. Assumes that the list is not
  /// empty.
  void setLastChunk(Chunk *last) {
    assert(!empty() && "cannot set the last chunk of an empty list");
    first_->data[INITIAL_CAPACITY] = (Node *)(void *)last;
  }
};

/// Save and restore an ESTree::ASTList.
class SaveASTList {
  ESTree::ASTList &list_;
  ESTree::ASTList saved_;

 public:
  SaveASTList(ESTree::ASTList &list) : list_(list), saved_(std::move(list)) {}

  ~SaveASTList() {
    list_ = std::move(saved_);
  }
};

} // namespace ESTree
} // namespace hermes

namespace std {
inline void swap(hermes::ESTree::ASTList &a, hermes::ESTree::ASTList &b) {
  a.swap(b);
}
} // namespace std

#endif // HERMES_AST_ASTLIST_H
