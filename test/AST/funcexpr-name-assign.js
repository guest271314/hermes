/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc --dump-transformed-ast %s 2>&1 ) | %FileCheck --match-full-lines %s

(function foo() {
    foo = 10;
//CHECK: {{.*}}funcexpr-name-assign.js:11:5: warning: assignment to read-only function expression name
//CHECK-NEXT:     foo = 10;
//CHECK-NEXT:     ^~~

})()

(function bar() {
    "use strict";
    bar = 10;
//CHECK: {{.*}}funcexpr-name-assign.js:20:5: error: assignment to read-only function expression name
//CHECK-NEXT:     bar = 10;
//CHECK-NEXT:     ^~~

})()
