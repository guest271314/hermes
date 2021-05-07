/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes          -O0                                         %s | %FileCheck %s --match-full-lines
// RUN: %hermes          -O                                          %s | %FileCheck %s --match-full-lines
// RUN: %hermes          -O0 -fobject-scoping %s | %FileCheck %s --match-full-lines
// RUN: %hermes          -O  -fobject-scoping %s | %FileCheck %s --match-full-lines
// RUN: %hermes --strict -O0                                         %s | %FileCheck %s --match-full-lines --check-prefix=CHKS
// RUN: %hermes --strict -O                                          %s | %FileCheck %s --match-full-lines --check-prefix=CHKS
// RUN: %hermes --strict -O0 -fobject-scoping %s | %FileCheck %s --match-full-lines --check-prefix=CHKS
// RUN: %hermes --strict -O  -fobject-scoping %s | %FileCheck %s --match-full-lines --check-prefix=CHKS

print("start");
//CHECK-LABEL: start
//CHKS-LABEL: start

const ro = 10;
try {
    (function foo(){
        foo = 10;
        print(typeof foo);
    })();
} catch (e) {
    print(e);
}
//CHECK-NEXT: function
//CHKS-NEXT: TypeError: Assignment to constant variable{{.*}}

try {
    ro = 10;
} catch (e) {
    print(e);
}
//CHECK-NEXT: TypeError: Assignment to constant variable{{.*}}
//CHKS-NEXT: TypeError: Assignment to constant variable{{.*}}
