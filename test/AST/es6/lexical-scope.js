/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc --dump-transformed-ast %s 2>&1 ) | %FileCheck --match-full-lines %s

// This is OK because let is in a different scope.
var a;
{
    let a;
}

// Invalid redeclaration in the same scope.
var b;
let b;
//CHECK: {{.*}}lexical-scope.js:18:5: error: Identifier 'b' is already declared
//CHECK: {{.*}}lexical-scope.js:17:5: note: previous declaration

// Invalid redeclaration in the same scope.
let c;
{
    var c;
//CHECK: {{.*}}lexical-scope.js:25:9: error: Identifier 'c' is already declared
//CHECK: {{.*}}lexical-scope.js:23:5: note: previous declaration
}

// Invalid redeclaration because the var shadows the let.
{
    let d;
    {
        var d;
//CHECK: {{.*}}lexical-scope.js:34:13: error: Identifier 'd' is already declared
//CHECK: {{.*}}lexical-scope.js:32:9: note: previous declaration
    }
}
