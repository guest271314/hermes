/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -w %s | %FileCheck --match-full-lines %s

print("BEGIN");
//CHECK: BEGIN

// In non-strict mode, the nested function redeclares the outer one.
(function () {
    bar();
    function bar() { print("bar1"); }
    {
        bar();
        function bar() { print("bar2"); }
    }
    bar();
})();
//CHECK-NEXT: bar1
//CHECK-NEXT: bar2
//CHECK-NEXT: bar2

// In strict mode, the nested function is separate.
(function () {
    "use strict";
    bar();
    function bar() { print("bar1"); }
    {
        bar();
        function bar() { print("bar2"); }
    }
    bar();
})();
//CHECK-NEXT: bar1
//CHECK-NEXT: bar2
//CHECK-NEXT: bar1

// Block-scope function variable is visible globally.
(function () {
    print(inner);
    {
        function inner() {}
    }
    print(typeof inner);
})();
//CHECK-NEXT: undefined
//CHECK-NEXT: function

// Block-scope function variable is only visible locally in strict mode.
try {
    (function () {
        "use strict";
        print(inner);
        {
            function inner() {}
        }
    })();
} catch (e) {
    print("caught", e.name);
}
//CHECK-NEXT: caught ReferenceError

// Block-scope function variable shadowed.
try {
    (function () {
        print(inner);
        {
            let inner = 10;
            {
                function inner() {}
            }
        }
    })();
} catch (e) {
    print("caught", e.name);
}
//CHECK-NEXT: caught ReferenceError
