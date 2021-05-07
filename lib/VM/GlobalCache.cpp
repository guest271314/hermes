/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/GlobalCache.h"

namespace hermes {
namespace vm {

GlobalCache::GlobalCache() {
  // Don't allocate with new, because we don't want to consume memory if the
  // cache is not used at all.
  bytesToUnmap_ =
      llvh::alignTo(sizeof(Entry) * kNumCacheEntries, oscompat::page_size());
  auto result = oscompat::vm_allocate(bytesToUnmap_);
  if (!result)
    hermes_fatal("failed to allocate global cache");
  entries_ = (Entry *)*result;
}

GlobalCache::~GlobalCache() {
  oscompat::vm_free(entries_, bytesToUnmap_);
}

void GlobalCache::markWeakRoots(WeakRootAcceptor &acceptor) {
  // Save some time if the cache has never been used.
  if (!weakUsed_)
    return;
  for (unsigned i = 0; i != kNumCacheEntries; ++i)
    acceptor.acceptWeak(entries_[i].weakValue);
}

} // namespace vm
} // namespace hermes
