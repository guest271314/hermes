/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (%hermes -enable-eval=true %s 2>&1 || true) | %FileCheck --match-full-lines %s

"use strict";

function foo () {
    arguments = 0;
//CHECK: {{.*}}invalid-args-eval.js:13:5: error: assignment to 'arguments'
//CHECK-NEXT:     arguments = 0;
//CHECK-NEXT:     ^~~~~~~~~

    eval = 0;
//CHECK: {{.*}}invalid-args-eval.js:18:5: error: assignment to 'eval'
//CHECK-NEXT:     eval = 0;
//CHECK-NEXT:     ^~~~

    ++arguments;
//CHECK: {{.*}}invalid-args-eval.js:23:7: error: assignment to 'arguments'
//CHECK-NEXT:     ++arguments;
//CHECK-NEXT:       ^~~~~~~~~

    eval++;
//CHECK: {{.*}}invalid-args-eval.js:28:5: error: assignment to 'eval'
//CHECK-NEXT:     eval++;
//CHECK-NEXT:     ^~~~

    arguments += 1;
//CHECK: {{.*}}invalid-args-eval.js:33:5: error: assignment to 'arguments'
//CHECK-NEXT:     arguments += 1;
//CHECK-NEXT:     ^~~~~~~~~

    eval -= 1;
//CHECK: {{.*}}invalid-args-eval.js:38:5: error: assignment to 'eval'
//CHECK-NEXT:     eval -= 1;
//CHECK-NEXT:     ^~~~

}

function bar () {
    var arguments, eval;
//CHECK: {{.*}}invalid-args-eval.js:46:9: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT:     var arguments, eval;
//CHECK-NEXT:         ^~~~~~~~~
//CHECK: {{.*}}invalid-args-eval.js:46:20: error: cannot declare 'eval' in strict mode
//CHECK-NEXT:     var arguments, eval;
//CHECK-NEXT:                    ^~~~

    var eval;
//CHECK: {{.*}}invalid-args-eval.js:54:9: error: cannot declare 'eval' in strict mode
//CHECK-NEXT:     var eval;
//CHECK-NEXT:         ^~~~

}

function baz(arguments, eval) { }
//CHECK: {{.*}}invalid-args-eval.js:61:14: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: function baz(arguments, eval) { }
//CHECK-NEXT:              ^~~~~~~~~
//CHECK: {{.*}}invalid-args-eval.js:61:25: error: cannot declare 'eval' in strict mode
//CHECK-NEXT: function baz(arguments, eval) { }
//CHECK-NEXT:                         ^~~~

var v1 = function (arguments) { }
//CHECK: {{.*}}invalid-args-eval.js:69:20: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: var v1 = function (arguments) { }
//CHECK-NEXT:                    ^~~~~~~~~

function arguments () {}
//CHECK: {{.*}}invalid-args-eval.js:74:10: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: function arguments () {}
//CHECK-NEXT:          ^~~~~~~~~
//CHECK: {{.*}}invalid-args-eval.js:74:10: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: function arguments () {}
//CHECK-NEXT:          ^~~~~~~~~

var v2 = function arguments () {}
//CHECK: {{.*}}invalid-args-eval.js:82:19: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: var v2 = function arguments () {}
//CHECK-NEXT:                   ^~~~~~~~~
//CHECK: {{.*}}invalid-args-eval.js:82:19: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: var v2 = function arguments () {}
//CHECK-NEXT:                   ^~~~~~~~~

var v3 = function eval () {}
//CHECK: {{.*}}invalid-args-eval.js:90:19: error: cannot declare 'eval' in strict mode
//CHECK-NEXT: var v3 = function eval () {}
//CHECK-NEXT:                   ^~~~
//CHECK: {{.*}}invalid-args-eval.js:90:19: error: cannot declare 'eval' in strict mode
//CHECK-NEXT: var v3 = function eval () {}
//CHECK-NEXT:                   ^~~~

for(var arguments = 0; arguments < 10; ) {}
//CHECK: {{.*}}invalid-args-eval.js:98:9: error: cannot declare 'arguments' in strict mode
//CHECK-NEXT: for(var arguments = 0; arguments < 10; ) {}
//CHECK-NEXT:         ^~~~~~~~~

for(var eval = 0; eval < 10; ) {}
//CHECK: {{.*}}invalid-args-eval.js:103:9: error: cannot declare 'eval' in strict mode
//CHECK-NEXT: for(var eval = 0; eval < 10; ) {}
//CHECK-NEXT:         ^~~~
//CHECK-NEXT: <unknown>:0: error: too many errors emitted
