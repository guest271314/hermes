/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

print("START");
//CHECK-LABEL: START

var funcs = []

function genFuncs() {
    for(var i = 0; i != 5; ++i) {
        try {
            throw i;
        } catch (i) {
            funcs.push(function() { return i; });
        }
    }
}

genFuncs();
for(var f of funcs)
    print(f());
//CHECK-NEXT: 0
//CHECK-NEXT: 1
//CHECK-NEXT: 2
//CHECK-NEXT: 3
//CHECK-NEXT: 4
