/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -fobject-scoping %s | %FileCheck --match-full-lines %s

print("start");
//CHECK: start

var obj = {};

try {
    const x = 10;
    with (obj) {
        (function myfunc() {
            myfunc = "notmyfunc";   // This should not throw
            print(typeof myfunc);   // Prove that the write didn't work
//CHECK-NEXT: function
            x = 20;                 // This should throw
        })()
    }
} catch (e) {
    print(e);
}
//CHECK-NEXT: TypeError: Assignment to constant variable x
