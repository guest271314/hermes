/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -w %s | %FileCheck --match-full-lines %s

(function foo() {
    print(typeof foo);
//CHECK: function
    foo = 10;
    print(typeof foo);
//CHECK: function
})()

