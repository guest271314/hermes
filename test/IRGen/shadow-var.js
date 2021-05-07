/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O0 -dump-ir %s | %FileCheck %s --match-full-lines
// RUN: %hermes %s | %FileCheck %s --match-full-lines --check-prefix=CHK2

print("start");
//CHK2: start

function foo1(x) {
    var x;
    return x;
}
//CHECK-LABEL:function foo1(x)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S2{p:%S1@global, v:[x]}
//CHECK-NEXT:  %2 = StoreVariableInst undefined : undefined, [x%S2], %1 : object, %S2
//CHECK-NEXT:  %3 = StoreVariableInst %x, [x%S2], %1 : object, %S2
//CHECK-NEXT:  %4 = LoadVariableInst [x%S2], %1 : object, %S2
//CHECK-NEXT:  %5 = ReturnInst %4
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %6 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

print(foo1("param"));
//CHK2-NEXT: param

function foo2(x) {
    var x = "var";
    return x;
}

print(foo2("param"));
//CHK2-NEXT: var

//CHECK-LABEL:function foo2(x)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S3{p:%S1@global, v:[x]}
//CHECK-NEXT:  %2 = StoreVariableInst undefined : undefined, [x%S3], %1 : object, %S3
//CHECK-NEXT:  %3 = StoreVariableInst %x, [x%S3], %1 : object, %S3
//CHECK-NEXT:  %4 = StoreVariableInst "var" : string, [x%S3], %1 : object, %S3
//CHECK-NEXT:  %5 = LoadVariableInst [x%S3], %1 : object, %S3
//CHECK-NEXT:  %6 = ReturnInst %5
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %7 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end
