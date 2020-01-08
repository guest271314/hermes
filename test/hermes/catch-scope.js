/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

print("BEGIN");
//CHECK: BEGIN

function foo() {
    // This should be resolved as the "var" declared within the catch.
    e1 = 20;
    try {
        throw "hello";
    } catch (e1) {
        // This "var" declares a function scoped variable, but the identifier
        // within the catch scope is resolved as the catch variable.
        var e1 = 10;
        print(e1);
//CHECK-NEXT: 10
    }
    // This should be resolved as the "var" declared within the catch.
    print(e1);
//CHECK-NEXT: 20
}

foo();
// Make sure that the assignment in foo() didn't create a global.
print(typeof e1);
//CHECK-NEXT: undefined
