/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 -dump-ir %s | %FileCheck %s --match-full-lines

// Use the closest available scope when accessing variables.
let v1 = 10;

function foo(p1) {
    try {
        throw 1;
    } catch (e) {
        return e + p1 + v1;
    }
}
//CHECK-LABEL:function foo(p1)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S2{p:%S1@global, v:[p1]}
//CHECK-NEXT:  %2 = StoreVariableInst %p1, [p1%S2], %1 : object, %S2
//CHECK-NEXT:  %3 = TryStartInst %BB1, %BB2
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %4 = CatchInst
//CHECK-NEXT:  %5 = CreateScopeInst %1 : object, %S3{p:%S2, v:[e]}
//CHECK-NEXT:  %6 = StoreVariableInst %4, [e%S3], %5 : object, %S3
//CHECK-NEXT:  %7 = LoadVariableInst [e%S3], %5 : object, %S3
//CHECK-NEXT:  %8 = LoadVariableInst [p1%S2], %1 : object, %S2
//CHECK-NEXT:  %9 = BinaryOperatorInst '+', %7, %8
//CHECK-NEXT:  %10 = LoadVariableInst [v1%S1@global], %0 : object, %S1@global
//CHECK-NEXT:  %11 = ThrowIfEmptyInst %10
//CHECK-NEXT:  %12 = BinaryOperatorInst '+', %9, %11
//CHECK-NEXT:  %13 = ReturnInst %12
