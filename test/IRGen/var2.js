/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O0 -dump-ir -dump-ir-types=0 %s | %FileCheck %s --match-full-lines
// RUN: %hermesc -O0 -dump-ir -dump-ir-types=0 -fobject-scoping %s | %FileCheck %s --match-full-lines --check-prefix=CHKDYN
// RUN: %hermesc -O0 -dump-ir -dump-ir-types=0 --strict %s | %FileCheck %s --match-full-lines --check-prefix=CHKS
// RUN: %hermesc -O0 -dump-ir -dump-ir-types=0 -fobject-scoping --strict %s | %FileCheck %s --match-full-lines --check-prefix=CHKSDYN

function outer() {
    (function foo(){
        foo = 10;
        print(typeof foo);
    })();
}
//CHECK-LABEL:function outer()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global}
//CHECK-NEXT:  %2 = CreateScopeInst %1, %S3{p:%S2, v:[foo]}
//CHECK-NEXT:  %3 = CreateFunctionInst %foo(), %2, %S3
//CHECK-NEXT:  %4 = StoreVariableInst %3, [foo%S3], %2, %S3
//CHECK-NEXT:  %5 = CallInst %3, undefined

//CHECK-LABEL:function foo()
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = GetFunctionParentScopeInst %S3@outer
//CHECK-NEXT:  %1 = CreateScopeInst %0, %S4{p:%S3@outer}
//CHECK-NEXT:  %2 = TryLoadGlobalPropertyInst globalObject, "print"
//CHECK-NEXT:  %3 = LoadVariableInst [foo%S3@outer], %0, %S3@outer
//CHECK-NEXT:  %4 = UnaryOperatorInst 'typeof', %3
//CHECK-NEXT:  %5 = CallInst %2, undefined, %4

//CHKDYN-LABEL:function outer()
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global, o:static}
//CHKDYN-NEXT:  %2 = CreateScopeInst %1, %S3{p:%S2, o:static, v:[foo]}
//CHKDYN-NEXT:  %3 = CreateFunctionInst %foo(), %2, %S3
//CHKDYN-NEXT:  %4 = StoreVariableInst %3, [foo%S3], %2, %S3
//CHKDYN-NEXT:  %5 = ReadOnlyVariableInst false, [foo%S3], %2, %S3
//CHKDYN-NEXT:  %6 = CallInst %3, undefined

//CHKDYN-LABEL:function foo()
//CHKDYN-NEXT:%BB0:
//CHKDYN-NEXT:  %0 = GetFunctionParentScopeInst %S3@outer
//CHKDYN-NEXT:  %1 = CreateScopeInst %0, %S4{p:%S3@outer, o:static}
//CHKDYN-NEXT:  %2 = TryLoadGlobalPropertyInst globalObject, "print"
//CHKDYN-NEXT:  %3 = LoadVariableInst [foo%S3@outer], %0, %S3@outer
//CHKDYN-NEXT:  %4 = UnaryOperatorInst 'typeof', %3
//CHKDYN-NEXT:  %5 = CallInst %2, undefined, %4

//CHKS-LABEL:function outer()
//CHKS-NEXT:%BB0:
//CHKS-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKS-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global}
//CHKS-NEXT:  %2 = CreateScopeInst %1, %S3{p:%S2, v:[foo]}
//CHKS-NEXT:  %3 = CreateFunctionInst %foo(), %2, %S3
//CHKS-NEXT:  %4 = StoreVariableInst %3, [foo%S3], %2, %S3
//CHKS-NEXT:  %5 = CallInst %3, undefined

//CHKS-LABEL:function foo()
//CHKS-NEXT:%BB0:
//CHKS-NEXT:  %0 = GetFunctionParentScopeInst %S3@outer
//CHKS-NEXT:  %1 = CreateScopeInst %0, %S4{p:%S3@outer}
//CHKS-NEXT:  %2 = CallBuiltinInst [HermesBuiltin.throwTypeError], undefined, "Assignment to constant variable"
//CHKS-NEXT:  %3 = TryLoadGlobalPropertyInst globalObject, "print"
//CHKS-NEXT:  %4 = LoadVariableInst [foo%S3@outer], %0, %S3@outer
//CHKS-NEXT:  %5 = UnaryOperatorInst 'typeof', %4
//CHKS-NEXT:  %6 = CallInst %3, undefined, %5

//CHKSDYN-LABEL:function outer()
//CHKSDYN-NEXT:%BB0:
//CHKSDYN-NEXT:  %0 = GetFunctionParentScopeInst %S1@global
//CHKSDYN-NEXT:  %1 = CreateScopeInst %0, %S2{p:%S1@global, o:static}
//CHKSDYN-NEXT:  %2 = CreateScopeInst %1, %S3{p:%S2, o:static, v:[foo]}
//CHKSDYN-NEXT:  %3 = CreateFunctionInst %foo(), %2, %S3
//CHKSDYN-NEXT:  %4 = StoreVariableInst %3, [foo%S3], %2, %S3
//CHKSDYN-NEXT:  %5 = ReadOnlyVariableInst false, [foo%S3], %2, %S3
//CHKSDYN-NEXT:  %6 = CallInst %3, undefined

//CHKSDYN-LABEL:function foo()
//CHKSDYN-NEXT:%BB0:
//CHKSDYN-NEXT:  %0 = GetFunctionParentScopeInst %S3@outer
//CHKSDYN-NEXT:  %1 = CreateScopeInst %0, %S4{p:%S3@outer, o:static}
//CHKSDYN-NEXT:  %2 = CallBuiltinInst [HermesBuiltin.throwTypeError], undefined, "Assignment to constant variable"
//CHKSDYN-NEXT:  %3 = TryLoadGlobalPropertyInst globalObject, "print"
//CHKSDYN-NEXT:  %4 = LoadVariableInst [foo%S3@outer], %0, %S3@outer
//CHKSDYN-NEXT:  %5 = UnaryOperatorInst 'typeof', %4
//CHKSDYN-NEXT:  %6 = CallInst %3, undefined, %5
