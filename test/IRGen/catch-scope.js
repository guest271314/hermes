/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc --dump-ir -O0 %s | %FileCheck --match-full-lines %s

function foo() {
    // This should be resolved as the "var" declared within the catch.
    e1 = 20;
    try {
        throw "hello";
    } catch (e1) {
        // This "var" declares a function scoped variable, but the identifier
        // within the catch scope is resolved as the catch variable.
        var e1 = 10;
        print(e1);
    }
    // This should be resolved as the "var" declared within the catch.
    print(e1);
}

foo();
// Make sure that the assignment in foo() didn't create a global.
print(typeof e1);

//CHECK-LABEL: function foo()
//CHECK-NEXT: frame = [e1, ?anon_0_e1]
//CHECK-NEXT: %BB0:
//CHECK-NEXT:   %0 = StoreFrameInst undefined : undefined, [e1]
//CHECK-NEXT:   %1 = StoreFrameInst 20 : number, [e1]
//CHECK-NEXT:   %2 = TryStartInst %BB1, %BB2
//CHECK-NEXT: %BB1:
//CHECK-NEXT:   %3 = CatchInst
//CHECK-NEXT:   %4 = StoreFrameInst %3, [?anon_0_e1]
//CHECK-NEXT:   %5 = StoreFrameInst 10 : number, [?anon_0_e1]
//CHECK-NEXT:   %6 = TryLoadGlobalPropertyInst globalObject : object, "print" : string
//CHECK-NEXT:   %7 = LoadFrameInst [?anon_0_e1]
//CHECK-NEXT:   %8 = CallInst %6, undefined : undefined, %7
//CHECK-NEXT:   %9 = BranchInst %BB3
//CHECK-NEXT: %BB3:
//CHECK-NEXT:   %10 = TryLoadGlobalPropertyInst globalObject : object, "print" : string
//CHECK-NEXT:   %11 = LoadFrameInst [e1]
//CHECK-NEXT:   %12 = CallInst %10, undefined : undefined, %11
//CHECK-NEXT:   %13 = ReturnInst undefined : undefined
//CHECK-NEXT: %BB2:
//CHECK-NEXT:   %14 = ThrowInst "hello" : string
//CHECK-NEXT: %BB4:
//CHECK-NEXT:   %15 = BranchInst %BB5
//CHECK-NEXT: %BB5:
//CHECK-NEXT:   %16 = TryEndInst
//CHECK-NEXT:   %17 = BranchInst %BB3
//CHECK-NEXT: function_end
