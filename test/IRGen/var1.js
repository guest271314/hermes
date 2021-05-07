/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O0 -dump-ir %s | %FileCheck %s --match-full-lines

function outer(a) {
    function inner1(b) {
        return a + b;
    }
    return inner1;
}
//CHECK-LABEL:function outer(a)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S0@global
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S1{p:%S0@global, v:[inner1, a]}
//CHECK-NEXT:  %2 = StoreVariableInst %a, [a%S1], %1 : object, %S1
//CHECK-NEXT:  %3 = CreateFunctionInst %inner1(), %1 : object, %S1
//CHECK-NEXT:  %4 = StoreVariableInst %3 : closure, [inner1%S1], %1 : object, %S1
//CHECK-NEXT:  %5 = LoadVariableInst [inner1%S1], %1 : object, %S1
//CHECK-NEXT:  %6 = ReturnInst %5
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %7 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

//CHECK-LABEL:function inner1(b)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@outer
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S2{p:%S1@outer, v:[b]}
//CHECK-NEXT:  %2 = StoreVariableInst %b, [b%S2], %1 : object, %S2
//CHECK-NEXT:  %3 = LoadVariableInst [a%S1@outer], %1 : object, %S2
//CHECK-NEXT:  %4 = LoadVariableInst [b%S2], %1 : object, %S2
//CHECK-NEXT:  %5 = BinaryOperatorInst '+', %3, %4
//CHECK-NEXT:  %6 = ReturnInst %5
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %7 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

function catch_scope(e) {
    try {
        if (e)
            throw "e is true";
    } catch (e) {
        return function inner2() { return e; }
    }
    return function inner3() { return e; }
}
//CHECK-LABEL:function catch_scope(e)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S0@global
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S3{p:%S0@global, v:[e]}
//CHECK-NEXT:  %2 = StoreVariableInst %e, [e%S3], %1 : object, %S3
//CHECK-NEXT:  %3 = TryStartInst %BB1, %BB2
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %4 = CatchInst
//CHECK-NEXT:  %5 = CreateScopeInst %1 : object, %S4{p:%S3, v:[e]}
//CHECK-NEXT:  %6 = StoreVariableInst %4, [e%S4], %5 : object, %S4
//CHECK-NEXT:  %7 = CreateScopeInst %5 : object, %S5{p:%S4, v:[inner2]}
//CHECK-NEXT:  %8 = CreateFunctionInst %inner2(), %7 : object, %S5
//CHECK-NEXT:  %9 = StoreVariableInst %8 : closure, [inner2%S5], %7 : object, %S5
//CHECK-NEXT:  %10 = ReturnInst %8 : closure
//CHECK-NEXT:%BB3:
//CHECK-NEXT:  %11 = CreateScopeInst %1 : object, %S6{p:%S3, v:[inner3]}
//CHECK-NEXT:  %12 = CreateFunctionInst %inner3(), %11 : object, %S6
//CHECK-NEXT:  %13 = StoreVariableInst %12 : closure, [inner3%S6], %11 : object, %S6
//CHECK-NEXT:  %14 = ReturnInst %12 : closure
//CHECK-NEXT:%BB2:
//CHECK-NEXT:  %15 = LoadVariableInst [e%S3], %1 : object, %S3
//CHECK-NEXT:  %16 = CondBranchInst %15, %BB4, %BB5
//CHECK-NEXT:%BB4:
//CHECK-NEXT:  %17 = ThrowInst "e is true" : string
//CHECK-NEXT:%BB5:
//CHECK-NEXT:  %18 = BranchInst %BB6
//CHECK-NEXT:%BB6:
//CHECK-NEXT:  %19 = BranchInst %BB7
//CHECK-NEXT:%BB8:
//CHECK-NEXT:  %20 = BranchInst %BB6
//CHECK-NEXT:%BB7:
//CHECK-NEXT:  %21 = TryEndInst
//CHECK-NEXT:  %22 = BranchInst %BB3
//CHECK-NEXT:%BB9:
//CHECK-NEXT:  %23 = BranchInst %BB3
//CHECK-NEXT:%BB10:
//CHECK-NEXT:  %24 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

//CHECK-LABEL:function inner2()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S5@catch_scope
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S7{p:%S5@catch_scope}
//CHECK-NEXT:  %2 = LoadVariableInst [e%S4@catch_scope], %1 : object, %S7
//CHECK-NEXT:  %3 = ReturnInst %2
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %4 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

//CHECK-LABEL:function inner3()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S6@catch_scope
//CHECK-NEXT:  %1 = CreateScopeInst %0 : object, %S8{p:%S6@catch_scope}
//CHECK-NEXT:  %2 = LoadVariableInst [e%S3@catch_scope], %1 : object, %S8
//CHECK-NEXT:  %3 = ReturnInst %2
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %4 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end
