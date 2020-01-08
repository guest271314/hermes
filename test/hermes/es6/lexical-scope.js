/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s

print("lexical-scope");
// CHECK: lexical-scope

(function () {
    print(x);
// CHECK-NEXT: undefined
    {
        let x = 10;
        print(x);
// CHECK-NEXT: 10
    }
    var x;
    print(x);
// CHECK-NEXT: undefined
})();


(function () {
    print(typeof x);
// CHECK-NEXT: undefined
    {
        let x = 10;
        print(typeof x);
// CHECK-NEXT: number
    }
    print(typeof x);
// CHECK-NEXT: undefined
})();

print("\nfor var")
//CHECK-LABEL: for var
var foo = {}
print(typeof foo);
// CHECK-NEXT: object
for(var foo = 0; foo < 1; ++foo)
    print(typeof foo);
// CHECK-NEXT: number
print(typeof foo);
// CHECK-NEXT: number

print("\nfor let")
//CHECK-LABEL: for let
foo = {}
print(typeof foo);
// CHECK-NEXT: object
for(let foo = 0; foo < 1; ++foo)
    print(typeof foo);
// CHECK-NEXT: number
print(typeof foo);
// CHECK-NEXT: object

print("\nfor let in")
//CHECK-LABEL: for let in
for(let foo in [1])
    print(typeof foo);
// CHECK-NEXT: string
print(typeof foo);
// CHECK-NEXT: object

print("\nfor let of")
//CHECK-LABEL: for let of
for(let foo of [1])
    print(typeof foo);
// CHECK-NEXT: number
print(typeof foo);
// CHECK-NEXT: object
