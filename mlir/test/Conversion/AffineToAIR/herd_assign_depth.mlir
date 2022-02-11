// (c) Copyright 2022 Xilinx Inc.

// RUN: air-opt %s -affine-to-air='herd-assign-depth=1' | FileCheck %s
// CHECK: scf.parallel
// CHECK: air.launch_herd
#map0 = affine_map<(d0) -> (-d0 + 32)>
#map1 = affine_map<(d0, d1)[s0] -> (d0 * 1024 + s0 + d1)>
module attributes {torch.debug_module_name = "mmult"} {
  func @forward(%arg0: memref<1024x1024xi32>, %arg1: memref<1024x1024xi32>, %arg2: memref<?x?xi32>) {
    %c0_i32 = arith.constant 0 : i32
    %0 = memref.alloc() : memref<1024x1024xi32>
    linalg.fill(%c0_i32, %0) : i32, memref<1024x1024xi32> 
    %1 = memref.cast %arg2 : memref<?x?xi32> to memref<1024x1024xi32>
    linalg.copy(%0, %1) : memref<1024x1024xi32>, memref<1024x1024xi32> 
    %c64 = arith.constant 64 : index
    %c32 = arith.constant 32 : index
    %c1024 = arith.constant 1024 : index
    %c0 = arith.constant 0 : index
    scf.parallel (%arg3, %arg4) = (%c0, %c0) to (%c1024, %c1024) step (%c64, %c64) {
      scf.for %arg5 = %c0 to %c1024 step %c32 {
        scf.parallel (%arg6, %arg7) = (%c0, %c0) to (%c64, %c64) step (%c32, %c32) {
          scf.for %arg8 = %c0 to %c32 step %c32 {
            %2 = affine.apply #map0(%arg8)
            %3 = arith.addi %arg3, %arg6 : index
            %4 = arith.addi %arg5, %arg8 : index
            %5 = memref.subview %arg0[%3, %4] [%c32, %2] [1, 1] : memref<1024x1024xi32> to memref<?x?xi32, #map1>
            %6 = affine.apply #map0(%arg8)
            %7 = arith.addi %arg5, %arg8 : index
            %8 = arith.addi %arg4, %arg7 : index
            %9 = memref.subview %arg1[%7, %8] [%6, %c32] [1, 1] : memref<1024x1024xi32> to memref<?x?xi32, #map1>
            %10 = arith.addi %arg3, %arg6 : index
            %11 = arith.addi %arg4, %arg7 : index
            %12 = memref.subview %1[%10, %11] [%c32, %c32] [1, 1] : memref<1024x1024xi32> to memref<?x?xi32, #map1>
            %13 = memref.alloc(%c32, %2) : memref<?x?xi32, 2>
            %14 = memref.alloc(%6, %c32) : memref<?x?xi32, 2>
            %15 = memref.alloc() : memref<32x32xi32, 2>
            linalg.copy(%5, %13) : memref<?x?xi32, #map1>, memref<?x?xi32, 2> 
            linalg.copy(%9, %14) : memref<?x?xi32, #map1>, memref<?x?xi32, 2> 
            linalg.copy(%12, %15) : memref<?x?xi32, #map1>, memref<32x32xi32, 2> 
            linalg.matmul ins(%13, %14 : memref<?x?xi32, 2>, memref<?x?xi32, 2>) outs(%15 : memref<32x32xi32, 2>)
            linalg.copy(%15, %12) : memref<32x32xi32, 2>, memref<?x?xi32, #map1> 
            memref.dealloc %13 : memref<?x?xi32, 2>
            memref.dealloc %14 : memref<?x?xi32, 2>
            memref.dealloc %15 : memref<32x32xi32, 2>
          }
          scf.yield
        }
      }
      scf.yield
    }
    return
  }
}