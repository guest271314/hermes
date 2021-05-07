/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_GLOBALCACHE_H
#define HERMES_VM_GLOBALCACHE_H

#include "hermes/VM/WeakRef.h"

namespace hermes {
namespace vm {

/// The global cache is similar to the existing property cache, but is a
/// little (very little) more general purpose, shared, fixed size, and
/// aliased.
/// It is possible to implement property caching on top of it, but it would
/// be less efficient because of aliasing and contention for the same cache
/// entry between different instructions.
/// The goal of this is to enable caching in new kinds of instructions
/// without having to redesign CodeBlock.
/// The cache is mapped instead of malloc-ed, so it won't consume memory
/// until the first time it is actually used.
/// The immediate goal for the cache is to be used by the dynamic scoping
/// instructions.
class GlobalCache {
 public:
  struct Entry {
   private:
    friend class GlobalCache;

    /// Pointer to the bytecode instruction in memory.
    const void *ip;
    /// To uniquely identify a bytecode instruction within a VM, it is not
    /// sufficient to store its address, because new instructions can be
    /// generated dynamically by eval() or loading new bytecode, and old ones
    /// can be garbage collected. Additionally, bytecode can be de-duplicated
    /// between functions, so one bytecode instruction can belong to two
    /// different functions.
    /// So, in addition to the instruction address, we store a "unique function
    /// ID", which is itself constructed by combining the per-RuntimeModule
    /// function ID with a unique module ID (a 48-bit counter).
    /// So, the full key is: unique_module_id:function_id:instruction_address.
    uint64_t uniqueFunctionID;

    /// Arbitrary value stored by the owner.
    int32_t value1i32;
    /// Arbitrary value stored by the owner.
    uint32_t value2u32;
    /// Arbitrary weak pointer stored by the owner.
    WeakRoot<GCCell> weakValue;
  };

  GlobalCache(const GlobalCache &) = delete;
  void operator=(const GlobalCache &) = delete;

  explicit GlobalCache();
  ~GlobalCache();

  /// Should be called by the Runtime's markWeakRoots.
  void markWeakRoots(WeakRootAcceptor &acceptor);

  /// \return the cache entry corresponding to the specified IP. Note that
  ///     because of aliasing, it may not have been initialized by that
  ///     instruction. That has to further be checked by \c checkKey().
  Entry *getCacheEntry(const void *ip) {
    return entries_ + ((uintptr_t)ip % kNumCacheEntries);
  }

  /// \return true if the specified cache entry, which must have been obtained
  ///     by \c getCacheEntry(ip) was initialized by that instruction. It
  ///     essentially compares the cache key.
  static bool
  checkKey(const Entry *entry, const void *ip, uint64_t uniqueFunctionID) {
    return entry->ip == ip && entry->uniqueFunctionID == uniqueFunctionID;
  }

  static int32_t getValue1i32(const Entry *entry) {
    return entry->value1i32;
  }
  static uint32_t getValue2u32(const Entry *entry) {
    return entry->value2u32;
  }
  static const WeakRoot<GCCell> &getWeakValue(const Entry *entry) {
    return entry->weakValue;
  }

  /// Populate the specified cache entry and mark it as owned by the specified
  /// instruction.
  static void initEntry(
      Entry *entry,
      const void *ip,
      uint64_t uniqueFunctionID,
      int32_t value1i32,
      uint32_t value2u32) {
    entry->ip = ip;
    entry->uniqueFunctionID = uniqueFunctionID;
    entry->value1i32 = value1i32;
    entry->value2u32 = value2u32;
  }

  /// A helper for updating the weak reference field. It sets a flag that
  /// weak references are in use in the cache at all, which adds some overhead
  /// during GC.
  WeakRoot<GCCell> &getWeakValueForUpdate(Entry *entry) {
    weakUsed_ = true;
    return entry->weakValue;
  }

 private:
  /// Number of entries in the cache. Must be a power of 2.
  static constexpr unsigned kNumCacheEntries = 2048;

  /// Pointer to the cache entries.
  Entry *entries_;
  /// Bytes to unmap when destroying.
  unsigned bytesToUnmap_;
  /// Set to true if the weak references in the cache have been used and must
  /// be marked by the GC.
  bool weakUsed_ = false;
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_GLOBALCACHE_H
