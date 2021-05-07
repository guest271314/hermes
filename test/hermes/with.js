/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fobject-scoping -O %s | %FileCheck --match-full-lines %s

var a = "a0", b = "b0";
print("0:", a, b);
//CHECK:0: a0 b0

function foo() {
    var b = "b1";
    var obj = {a: "a2", b: "b2"};
    print("1:", a, b);
//CHECK-NEXT:1: a0 b1

    with (obj) {
        print("2:", a, b);
//CHECK-NEXT:2: a2 b2

        (function inner (){ 
            var a = "a3";
            print("3:", a, b);
//CHECK-NEXT:3: a3 b2

            delete obj.b;
            print("4:", a, b);
//CHECK-NEXT:4: a3 b1
        })();
        print("5:", a, b);
//CHECK-NEXT:5: a2 b1

        obj.b = "b2-again";
        print("6:", a, b);
//CHECK-NEXT:6: a2 b2-again
    }
    print("7:", a, b);
//CHECK-NEXT:7: a0 b1
}

foo();
