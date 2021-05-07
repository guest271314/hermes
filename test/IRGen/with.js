/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -fobject-scoping -O0 -dump-ir -dump-ir-types=0 %s | %FileCheck --match-full-lines %s

var a = "a0", b = "b0";
print("0:", a, b);

foo();

function foo() {
//CHECK-LABEL:function foo()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global, o:static, v:[b, obj]}
//CHECK-NEXT:  %2 = StoreVariableInst undefined, [b%S2], %1, %S2
//CHECK-NEXT:  %3 = StoreVariableInst undefined, [obj%S2], %1, %S2
    var b = "b1";
//CHECK-NEXT:  %4 = StoreVariableInst "b1", [b%S2], %1, %S2
    var obj = {a: "a2", b: "b2"};
//CHECK-NEXT:  %5 = AllocObjectInst 2, empty
//CHECK-NEXT:  %6 = StoreNewOwnPropertyInst "a2", %5, "a", true
//CHECK-NEXT:  %7 = StoreNewOwnPropertyInst "b2", %5, "b", true
//CHECK-NEXT:  %8 = StoreVariableInst %5, [obj%S2], %1, %S2
    print("1:", a, b);
//CHECK-NEXT:  %9 = TryLoadGlobalPropertyInst globalObject, "print"
//CHECK-NEXT:  %10 = TryLoadGlobalPropertyInst globalObject, "a"
//CHECK-NEXT:  %11 = LoadVariableInst [b%S2], %1, %S2
//CHECK-NEXT:  %12 = CallInst %9, undefined, "1:", %10, %11
    with (obj) {
//CHECK-NEXT:  %13 = LoadVariableInst [obj%S2], %1, %S2
//CHECK-NEXT:  %14 = CreateDynamicObjectScopeInst %1, %S3{p:%S2, o:with}, %13
        print("2:", a, b);
//CHECK-NEXT:  %15 = LoadDynamicInst true, "print", %14, %S3
//CHECK-NEXT:  %16 = LoadDynamicInst true, "a", %14, %S3
//CHECK-NEXT:  %17 = LoadDynamicInst true, "b", %14, %S3
//CHECK-NEXT:  %18 = CallInst %15, undefined, "2:", %16, %17
        (function inner(){ 
            var a = "a3";
            print("3:", a, b);
            delete obj.b;
            print("4:", a, b);
        })();
//CHECK-NEXT:  %19 = CreateScopeInst %14, %S4{p:%S3, o:static, v:[inner]}
//CHECK-NEXT:  %20 = CreateFunctionInst %inner(), %19, %S4
//CHECK-NEXT:  %21 = StoreVariableInst %20, [inner%S4], %19, %S4
//CHECK-NEXT:  %22 = ReadOnlyVariableInst false, [inner%S4], %19, %S4
//CHECK-NEXT:  %23 = CallInst %20, undefined
        print("5:", a, b);
//CHECK-NEXT:  %24 = LoadDynamicInst true, "print", %14, %S3
//CHECK-NEXT:  %25 = LoadDynamicInst true, "a", %14, %S3
//CHECK-NEXT:  %26 = LoadDynamicInst true, "b", %14, %S3
//CHECK-NEXT:  %27 = CallInst %24, undefined, "5:", %25, %26
        obj.b = "b2-again";
//CHECK-NEXT:  %28 = LoadDynamicInst true, "obj", %14, %S3
//CHECK-NEXT:  %29 = StorePropertyInst "b2-again", %28, "b"
        print("6:", a, b);
//CHECK-NEXT:  %30 = LoadDynamicInst true, "print", %14, %S3
//CHECK-NEXT:  %31 = LoadDynamicInst true, "a", %14, %S3
//CHECK-NEXT:  %32 = LoadDynamicInst true, "b", %14, %S3
//CHECK-NEXT:  %33 = CallInst %30, undefined, "6:", %31, %32
    }
    print("7:", a, b);
//CHECK-NEXT:  %34 = TryLoadGlobalPropertyInst globalObject, "print"
//CHECK-NEXT:  %35 = TryLoadGlobalPropertyInst globalObject, "a"
//CHECK-NEXT:  %36 = LoadVariableInst [b%S2], %1, %S2
//CHECK-NEXT:  %37 = CallInst %34, undefined, "7:", %35, %36
}

//CHECK-LABEL:function inner()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S4@foo
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S5{p:%S4@foo, o:static, v:[a]}
//CHECK-NEXT:  %2 = StoreVariableInst undefined, [a%S5], %1, %S5
//CHECK-NEXT:  %3 = StoreVariableInst "a3", [a%S5], %1, %S5
//CHECK-NEXT:  %4 = LoadDynamicInst true, "print", %0, %S4@foo
//CHECK-NEXT:  %5 = LoadVariableInst [a%S5], %1, %S5
//CHECK-NEXT:  %6 = LoadDynamicInst true, "b", %0, %S4@foo
//CHECK-NEXT:  %7 = CallInst %4, undefined, "3:", %5, %6
//CHECK-NEXT:  %8 = LoadDynamicInst true, "obj", %0, %S4@foo
//CHECK-NEXT:  %9 = DeletePropertyInst %8, "b"
//CHECK-NEXT:  %10 = LoadDynamicInst true, "print", %0, %S4@foo
//CHECK-NEXT:  %11 = LoadVariableInst [a%S5], %1, %S5
//CHECK-NEXT:  %12 = LoadDynamicInst true, "b", %0, %S4@foo
//CHECK-NEXT:  %13 = CallInst %10, undefined, "4:", %11, %12

