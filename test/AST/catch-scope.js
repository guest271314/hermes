/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc --dump-transformed-ast %s 2>&1 ) | %FileCheck --match-full-lines %s

try {
    throw "hello";
} catch (e1) {
    var e1 = 10;
    print(e1);
}

try {
    throw "hello";
} catch (e2) {
    let e2 = 10;
//CHECK: {{.*}}catch-scope.js:20:9: error: Identifier 'e2' is already declared
//CHECK: {{.*}}catch-scope.js:19:10: note: previous declaration
    print(e2);
}

try {
    throw ["hello"];
} catch ([e3]) {
    var e3 = 10;
//CHECK: {{.*}}catch-scope.js:29:9: error: Identifier 'e3' is already declared
//CHECK: {{.*}}catch-scope.js:28:11: note: previous declaration
    print(e3);
}
print(e3);
