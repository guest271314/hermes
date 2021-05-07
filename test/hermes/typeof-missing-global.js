/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes                                                  %s | %FileCheck %s --match-full-lines
// RUN: %hermes          -fobject-scoping %s | %FileCheck %s --match-full-lines
// RUN: %hermes --strict                                         %s | %FileCheck %s --match-full-lines --check-prefix=CHKS
// RUN: %hermes --strict -fobject-scoping %s | %FileCheck %s --match-full-lines --check-prefix=CHKS

print(typeof foo)
//CHECK-LABEL: undefined
//CHKS-LABEL: undefined
