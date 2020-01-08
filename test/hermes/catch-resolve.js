/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

print("BEGIN");
//CHECK: BEGIN

// Test that the inlined "finally" block resolves variables correcty.
function foo() {
    let x = "outer";
    try {
        do {
            let x = "inner";
            print(x);
            return;
        } while (false);
    } finally {
        print(x);
    }
}

foo();

//CHECK-NEXT: inner
//CHECK-NEXT: outer
