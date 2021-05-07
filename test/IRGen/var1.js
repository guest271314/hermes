/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O0 -dump-ir-types=0 -dump-ir %s | %FileCheck %s --match-full-lines
// RUN: %hermesc -O0 -dump-ir-types=0 -dump-ir -fobject-scoping %s | %FileCheck %s --match-full-lines --check-prefix=CHKDYN

// These two calls are for runtime testing, not relevant here.
print(catch_scope(true)());
print(catch_scope(0)());

function outer(a) {
    function inner1(b) {
        return a + b;
    }
    return inner1;
}
//CHECK-LABEL:function outer(a)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global, v:[inner1, a]}
//CHECK-NEXT:  %2 = StoreVariableInst %a, [a%S2], %1, %S2
//CHECK-NEXT:  %3 = CreateFunctionInst %inner1(), %1, %S2
//CHECK-NEXT:  %4 = StoreVariableInst %3, [inner1%S2], %1, %S2
//CHECK-NEXT:  %5 = LoadVariableInst [inner1%S2], %1, %S2
//CHECK-NEXT:  %6 = ReturnInst %5

//CHECK-LABEL:function inner1(b)
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S2@outer
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S3{p:%S2@outer, v:[b]}
//CHECK-NEXT:  %2 = StoreVariableInst %b, [b%S3], %1, %S3
//CHECK-NEXT:  %3 = LoadVariableInst [a%S2@outer], %0, %S2@outer
//CHECK-NEXT:  %4 = LoadVariableInst [b%S3], %1, %S3
//CHECK-NEXT:  %5 = BinaryOperatorInst '+', %3, %4
//CHECK-NEXT:  %6 = ReturnInst %5

//CHKDYN-LABEL:function outer(a)
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global, o:static, v:[inner1, a]}
//CHKDYN-NEXT:  %2 = StoreVariableInst %a, [a%S2], %1, %S2
//CHKDYN-NEXT:  %3 = CreateFunctionInst %inner1(), %1, %S2
//CHKDYN-NEXT:  %4 = StoreVariableInst %3, [inner1%S2], %1, %S2
//CHKDYN-NEXT:  %5 = LoadVariableInst [inner1%S2], %1, %S2
//CHKDYN-NEXT:  %6 = ReturnInst %5

//CHKDYN-LABEL:function inner1(b)
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S2@outer
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S3{p:%S2@outer, o:static, v:[b]}
//CHKDYN-NEXT:  %2 = StoreVariableInst %b, [b%S3], %1, %S3
//CHKDYN-NEXT:  %3 = LoadVariableInst [a%S2@outer], %0, %S2@outer
//CHKDYN-NEXT:  %4 = LoadVariableInst [b%S3], %1, %S3
//CHKDYN-NEXT:  %5 = BinaryOperatorInst '+', %3, %4
//CHKDYN-NEXT:  %6 = ReturnInst %5

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
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S4{p:%S1@global, v:[e]}
//CHECK-NEXT:  %2 = StoreVariableInst %e, [e%S4], %1, %S4
//CHECK-NEXT:  %3 = TryStartInst %BB1, %BB2
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %4 = CatchInst
//CHECK-NEXT:  %5 = CreateScopeInst %1, %S5{p:%S4, v:[e]}
//CHECK-NEXT:  %6 = StoreVariableInst %4, [e%S5], %5, %S5
//CHECK-NEXT:  %7 = CreateScopeInst %5, %S6{p:%S5, v:[inner2]}
//CHECK-NEXT:  %8 = CreateFunctionInst %inner2(), %7, %S6
//CHECK-NEXT:  %9 = StoreVariableInst %8, [inner2%S6], %7, %S6
//CHECK-NEXT:  %10 = ReturnInst %8
//CHECK-NEXT:%BB3:
//CHECK-NEXT:  %11 = CreateScopeInst %1, %S7{p:%S4, v:[inner3]}
//CHECK-NEXT:  %12 = CreateFunctionInst %inner3(), %11, %S7
//CHECK-NEXT:  %13 = StoreVariableInst %12, [inner3%S7], %11, %S7
//CHECK-NEXT:  %14 = ReturnInst %12
//CHECK-NEXT:%BB2:
//CHECK-NEXT:  %15 = LoadVariableInst [e%S4], %1, %S4
//CHECK-NEXT:  %16 = CondBranchInst %15, %BB4, %BB5

//CHECK-LABEL:function inner2()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S6@catch_scope
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S8{p:%S6@catch_scope}
//CHECK-NEXT:  %2 = LoadVariableInst [e%S5@catch_scope], %0, %S6@catch_scope
//CHECK-NEXT:  %3 = ReturnInst %2

//CHECK-LABEL:function inner3()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S7@catch_scope
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S9{p:%S7@catch_scope}
//CHECK-NEXT:  %2 = LoadVariableInst [e%S4@catch_scope], %0, %S7@catch_scope
//CHECK-NEXT:  %3 = ReturnInst %2

//CHKDYN-LABEL:function catch_scope(e)
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S4{p:%S1@global, o:static, v:[e]}
//CHKDYN-NEXT:  %2 = StoreVariableInst %e, [e%S4], %1, %S4
//CHKDYN-NEXT:  %3 = TryStartInst %BB1, %BB2
//CHKDYN-NEXT:%BB1:
//CHKDYN-NEXT:  %4 = CatchInst
//CHKDYN-NEXT:  %5 = CreateScopeInst %1, %S5{p:%S4, o:static, v:[e]}
//CHKDYN-NEXT:  %6 = StoreVariableInst %4, [e%S5], %5, %S5
//CHKDYN-NEXT:  %7 = CreateScopeInst %5, %S6{p:%S5, o:static, v:[inner2]}
//CHKDYN-NEXT:  %8 = CreateFunctionInst %inner2(), %7, %S6
//CHKDYN-NEXT:  %9 = StoreVariableInst %8, [inner2%S6], %7, %S6
//CHKDYN-NEXT:  %10 = ReadOnlyVariableInst false, [inner2%S6], %7, %S6
//CHKDYN-NEXT:  %11 = ReturnInst %8
//CHKDYN-NEXT:%BB3:
//CHKDYN-NEXT:  %12 = CreateScopeInst %1, %S7{p:%S4, o:static, v:[inner3]}
//CHKDYN-NEXT:  %13 = CreateFunctionInst %inner3(), %12, %S7
//CHKDYN-NEXT:  %14 = StoreVariableInst %13, [inner3%S7], %12, %S7
//CHKDYN-NEXT:  %15 = ReadOnlyVariableInst false, [inner3%S7], %12, %S7
//CHKDYN-NEXT:  %16 = ReturnInst %13
//CHKDYN-NEXT:%BB2:
//CHKDYN-NEXT:  %17 = LoadVariableInst [e%S4], %1, %S4
//CHKDYN-NEXT:  %18 = CondBranchInst %17, %BB4, %BB5

//CHKDYN-LABEL:function inner2()
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S6@catch_scope
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S8{p:%S6@catch_scope, o:static}
//CHKDYN-NEXT:  %2 = LoadVariableInst [e%S5@catch_scope], %0, %S6@catch_scope
//CHKDYN-NEXT:  %3 = ReturnInst %2

//CHKDYN-LABEL:function inner3()
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S7@catch_scope
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S9{p:%S7@catch_scope, o:static}
//CHKDYN-NEXT:  %2 = LoadVariableInst [e%S4@catch_scope], %0, %S7@catch_scope
//CHKDYN-NEXT:  %3 = ReturnInst %2

function typeofund() {
    return typeof foobar;
}
//CHECK-LABEL:function typeofund()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S10{p:%S1@global}
//CHECK-NEXT:  %2 = LoadPropertyInst globalObject, "foobar"
//CHECK-NEXT:  %3 = UnaryOperatorInst 'typeof', %2
//CHECK-NEXT:  %4 = ReturnInst %3

//CHKDYN-LABEL:function typeofund()
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S10{p:%S1@global, o:static}
//CHKDYN-NEXT:  %2 = LoadPropertyInst globalObject, "foobar"
//CHKDYN-NEXT:  %3 = UnaryOperatorInst 'typeof', %2
//CHKDYN-NEXT:  %4 = ReturnInst %3

function constvar() {
    const x = 10;
    return x;
}
//CHECK-LABEL:function constvar()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S11{p:%S1@global, v:[x]}
//CHECK-NEXT:  %2 = StoreVariableInst empty, [x%S11], %1, %S11
//CHECK-NEXT:  %3 = StoreVariableInst 10, [x%S11], %1, %S11
//CHECK-NEXT:  %4 = LoadVariableInst [x%S11], %1, %S11
//CHECK-NEXT:  %5 = ThrowIfEmptyInst %4
//CHECK-NEXT:  %6 = ReturnInst %5

//CHKDYN-LABEL:function constvar()
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S11{p:%S1@global, o:static, v:[x]}
//CHKDYN-NEXT:  %2 = StoreVariableInst empty, [x%S11], %1, %S11
//CHKDYN-NEXT:  %3 = StoreVariableInst 10, [x%S11], %1, %S11
//CHKDYN-NEXT:  %4 = ReadOnlyVariableInst true, [x%S11], %1, %S11
//CHKDYN-NEXT:  %5 = LoadVariableInst [x%S11], %1, %S11
//CHKDYN-NEXT:  %6 = ThrowIfEmptyInst %5
//CHKDYN-NEXT:  %7 = ReturnInst %6
