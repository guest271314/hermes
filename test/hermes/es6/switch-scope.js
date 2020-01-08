/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

// Test switch lexical scoping.

print("BEGIN");
//CHECK: BEGIN

let f1 = () => print(foo);
let f2;
let foo = "out";

// Constant case labels.

d = 1;
switch (d) {
    case 0:
        break;
    case 1:
        f2 = () => print(foo);
        let foo = "in";
        break;
}

f1();
//CHECK-NEXT: out
f2();
//CHECK-NEXT: in


f1 = () => print(bar);
let bar = "out";

e = 1;
switch (d) {
    case 0:
        break;
    case e:
        f2 = () => print(bar);
        let bar = "in";
        break;
}

f1();
//CHECK-NEXT: out
f2();
//CHECK-NEXT: in
