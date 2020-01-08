/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc --dump-transformed-ast %s 2>&1 ) | %FileCheck --match-full-lines %s

const a = 10;

++a;
//CHECK: {{.*}}const-assign.js:12:3: error: assignment to constant variable
//CHECK-NEXT: ++a;
//CHECK-NEXT:   ^

{
    let a = 20;
    ++a;
}

a = 20;
//CHECK: {{.*}}const-assign.js:22:1: error: assignment to constant variable
//CHECK-NEXT: a = 20;
//CHECK-NEXT: ^

for(a of [1,2]);
//CHECK: {{.*}}const-assign.js:27:5: error: assignment to constant variable
//CHECK-NEXT: for(a of [1,2]);
//CHECK-NEXT:     ^
