// (c) Copyright 2022 Xilinx Inc. All Rights Reserved.
//
// ===- AIRMiscPasses.cpp -------------------------------------------------===//
//
// Miscellaneous useful and/or experimental passes
//
// ===---------------------------------------------------------------------===//

#include "air/Transform/AIRMiscPasses.h"
#include "PassDetail.h"
#include "air/Dialect/AIR/AIRDialect.h"
#include "air/Transform/AIRTilingUtils.h"
#include "air/Util/Dependency.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/RegionUtils.h"

#include "llvm/Support/Debug.h"

#include <list>
#include <numeric>

#define DEBUG_TYPE "air-misc-passes"

using namespace mlir;

namespace {

class AIRExamplePass : public xilinx::air::AIRExamplePassBase<AIRExamplePass> {

public:
  AIRExamplePass() = default;
  AIRExamplePass(const AIRExamplePass &pass){};

  void runOnOperation() override;

private:
};

void AIRExamplePass::runOnOperation() {}

class AIRLinalgNamePass
    : public xilinx::air::AIRLinalgNamePassBase<AIRLinalgNamePass> {

public:
  AIRLinalgNamePass() = default;
  AIRLinalgNamePass(const AIRLinalgNamePass &pass){};

  void runOnOperation() override;

private:
};

void AIRLinalgNamePass::runOnOperation() {
  auto module = getOperation();
  auto ctx = module.getContext();

  unsigned id = 0;
  module.walk([&](linalg::LinalgOp op) {
    auto attr = op->getAttrOfType<StringAttr>(
        linalg::LinalgTransforms::kLinalgTransformMarker);
    if (!attr) {
      std::string name =
          op->getName().getStringRef().str() + std::to_string(id++);
      op->setAttr(linalg::LinalgTransforms::kLinalgTransformMarker,
                  StringAttr::get(ctx, name));
    }
  });
}

class AIRRemoveLinalgNamePass
    : public xilinx::air::AIRRemoveLinalgNamePassBase<AIRRemoveLinalgNamePass> {

public:
  AIRRemoveLinalgNamePass() = default;
  AIRRemoveLinalgNamePass(const AIRRemoveLinalgNamePass &pass){};

  void runOnOperation() override;

private:
};

void AIRRemoveLinalgNamePass::runOnOperation() {
  auto module = getOperation();

  module.walk([&](linalg::LinalgOp op) {
    auto attr = op->getAttrOfType<StringAttr>(
        linalg::LinalgTransforms::kLinalgTransformMarker);
    if (attr) {
      op->removeAttr(linalg::LinalgTransforms::kLinalgTransformMarker);
    }
  });
}

// AIRPromoteUniformL1Dma
class AIRPromoteUniformL1Dma
    : public xilinx::air::AIRPromoteUniformL1DmaBase<AIRPromoteUniformL1Dma> {

public:
  AIRPromoteUniformL1Dma() = default;
  AIRPromoteUniformL1Dma(const AIRPromoteUniformL1Dma &pass){};

  void runOnOperation() override;

private:
};

void do_clone(OpBuilder &builder, Operation *op,
              BlockAndValueMapping &mapping) {
  if (!op)
    return;
  for (auto o : op->getOperands()) {
    if (mapping.contains(o))
      continue;
    do_clone(builder, o.getDefiningOp(), mapping);
  }
  builder.clone(*op, mapping);
}

void AIRPromoteUniformL1Dma::runOnOperation() {
  auto module = getOperation();
  // auto ctx = module.getContext();

  std::vector<Operation *> erasedOps;
  int64_t max_id = -1;
  SmallVector<xilinx::air::DmaMemcpyNdOp, 16> memCopies;
  module.walk([&](xilinx::air::DmaMemcpyNdOp memcpyOp) {
    memCopies.push_back(memcpyOp);
    IntegerAttr attr = memcpyOp->getAttrOfType<IntegerAttr>("id");
    if (!attr)
      return;
    max_id = std::max(max_id, attr.getInt());
  });

  for (auto memcpyOp : memCopies) {
    auto pipeline = memcpyOp->getParentOfType<xilinx::air::HerdPipelineOp>();
    auto stage = memcpyOp->getParentOfType<xilinx::air::PipelineStageOp>();
    auto launch = memcpyOp->getParentOfType<xilinx::air::HerdLaunchOp>();
    if (!pipeline || !stage || !launch)
      continue;

    // auto direction = pipeline->getAttrOfType<StringAttr>("direction");
    auto uniform = stage->getAttrOfType<BoolAttr>("uniform");
    if (!uniform)
      continue;

    auto src_type = memcpyOp.src().getType().cast<MemRefType>();
    auto dst_type = memcpyOp.dst().getType().cast<MemRefType>();
    auto src_space = src_type.getMemorySpaceAsInt();
    auto dst_space = dst_type.getMemorySpaceAsInt();

    MemRefType ty = nullptr;
    bool to_l1 = (src_space == 0 && dst_space == 2);
    bool from_l1 = (src_space == 2 && dst_space == 0);
    if (to_l1)
      ty = dst_type;
    else if (from_l1)
      ty = src_type;
    else
      continue;

    OpBuilder builder(launch);
    auto loc = memcpyOp->getLoc();
    auto alloc = builder.create<memref::AllocOp>(
        loc, MemRefType::get(ty.getShape(), ty.getElementType(),
                             ty.getLayout().getAffineMap(), 1));
    std::vector<Value> launch_operands;
    BlockAndValueMapping remap;
    for (unsigned int i = 0; i < launch.getNumKernelOperands(); i++) {
      auto arg = launch.getKernelArguments()[i];
      auto oper = launch.getKernelOperand(i);
      remap.map(arg, oper);
    }
    if (to_l1)
      remap.map(memcpyOp.dst(), alloc);
    do_clone(builder, memcpyOp.getOperation(), remap);

    launch_operands.insert(launch_operands.begin(),
                           launch->getOperands().begin(),
                           launch->getOperands().end());
    launch_operands.push_back(alloc.getResult());
    launch->setOperands(launch_operands);
    launch.body().front().addArgument(alloc.getType(), loc);
    auto sizeAttr = launch->getAttr("operand_segment_sizes")
                        .cast<::mlir::DenseIntElementsAttr>();
    const uint32_t *it = &*sizeAttr.value_begin<uint32_t>();
    auto newAttr = DenseIntElementsAttr::get(sizeAttr.getType(),
                                             {it[0], it[1], it[2], it[3] + 1});
    launch->setAttr("operand_segment_sizes", newAttr);

    builder.setInsertionPoint(memcpyOp);
    SmallVector<Value, 2> opers{};
    SmallVector<Value, 2> mt;
    Value a = launch.getKernelArguments()[it[3]];
    builder.create<xilinx::air::DmaMemcpyNdOp>(
        loc, SmallVector<Type, 1>{}, mt, to_l1 ? memcpyOp.dst() : a, mt, mt, mt,
        to_l1 ? a : memcpyOp.src(), mt, mt, mt);
    erasedOps.push_back(memcpyOp);
  }
  for (auto e : erasedOps)
    e->erase();
}

// return true if op is a function of v
bool isFuncOf(Operation *op, Value v, std::vector<Operation *> &ops) {
  bool r = false;
  if (!op)
    return r;

  for (auto o : op->getOperands()) {
    if ((o == v) || (isFuncOf(o.getDefiningOp(), v, ops))) {
      if (std::find(std::begin(ops), std::end(ops), op) == std::end(ops))
        ops.push_back(op);
      r = true;
    }
  }
  return r;
}

// AIRSpecializeDma
class AIRSpecializeDma
    : public xilinx::air::AIRSpecializeDmaBase<AIRSpecializeDma> {

public:
  AIRSpecializeDma() = default;
  AIRSpecializeDma(const AIRSpecializeDma &pass){};

  void runOnOperation() override;

private:
};

void AIRSpecializeDma::runOnOperation() {
  auto module = getOperation();
  auto ctx = module.getContext();

  module.walk([&](xilinx::air::HerdLaunchOp launch) {
    launch.walk([&](xilinx::air::DmaMemcpyNdOp memcpyOp) {
      std::vector<Operation *> xOps, yOps;
      bool fn_x = isFuncOf(memcpyOp, launch.getTileIds().x, xOps);
      bool fn_y = isFuncOf(memcpyOp, launch.getTileIds().y, yOps);
      auto herd_size = launch.getHerdSizeOperands();
      int64_t herd_size_x =
          cast<arith::ConstantIndexOp>(herd_size.x.getDefiningOp()).value();
      int64_t herd_size_y =
          cast<arith::ConstantIndexOp>(herd_size.y.getDefiningOp()).value();
      if (fn_x && !fn_y) {
        auto loc = memcpyOp->getLoc();
        OpBuilder builder(memcpyOp);
        auto pipe = builder.create<xilinx::air::HerdPipelineOp>(loc);
        pipe->setAttr("direction", StringAttr::get(ctx, "horiz"));
        auto pipe_bb = new Block();
        pipe.body().push_back(pipe_bb);
        builder.setInsertionPointToEnd(pipe_bb);
        builder.create<xilinx::air::PipelineTerminatorOp>(
            loc, SmallVector<Value, 1>{});
        builder.setInsertionPointToStart(pipe_bb);
        for (int x = 0; x < herd_size_x; x++) {
          auto stage = builder.create<xilinx::air::PipelineStageOp>(
              loc, SmallVector<Type, 1>{}, SmallVector<Value, 1>{});
          stage->setAttr("uniform", BoolAttr::get(ctx, true));
          auto stage_bb = new Block();
          stage.body().push_back(stage_bb);
          auto stage_builder = OpBuilder::atBlockEnd(stage_bb);
          auto c_x = stage_builder.create<arith::ConstantIndexOp>(loc, x);
          BlockAndValueMapping remap;
          remap.map(launch.getTileIds().x, c_x);
          for (auto xop : xOps)
            stage_builder.clone(*xop, remap);
          stage_builder.create<xilinx::air::PipelineYieldOp>(
              loc, SmallVector<Type, 1>{}, SmallVector<Value, 1>{});
        }
        memcpyOp.erase();
      }
      if (fn_y && !fn_x) {
        auto loc = memcpyOp->getLoc();
        OpBuilder builder(memcpyOp);
        auto pipe = builder.create<xilinx::air::HerdPipelineOp>(loc);
        pipe->setAttr("direction", StringAttr::get(ctx, "vert"));
        auto pipe_bb = new Block();
        pipe.body().push_back(pipe_bb);
        builder.setInsertionPointToEnd(pipe_bb);
        builder.create<xilinx::air::PipelineTerminatorOp>(
            loc, SmallVector<Value, 1>{});
        builder.setInsertionPointToStart(pipe_bb);
        for (int y = 0; y < herd_size_y; y++) {
          auto stage = builder.create<xilinx::air::PipelineStageOp>(
              loc, SmallVector<Type, 1>{}, SmallVector<Value, 1>{});
          stage->setAttr("uniform", BoolAttr::get(ctx, true));
          auto stage_bb = new Block();
          stage.body().push_back(stage_bb);
          auto stage_builder = OpBuilder::atBlockEnd(stage_bb);
          auto c_y = stage_builder.create<arith::ConstantIndexOp>(loc, y);
          BlockAndValueMapping remap;
          remap.map(launch.getTileIds().y, c_y);
          for (auto yop : yOps)
            stage_builder.clone(*yop, remap);
          stage_builder.create<xilinx::air::PipelineYieldOp>(
              loc, SmallVector<Type, 1>{}, SmallVector<Value, 1>{});
        }
        memcpyOp.erase();
      }
    });
  });
}

// AIRSpecializeDmaBroadcast
class AIRSpecializeDmaBroadcast
    : public xilinx::air::AIRSpecializeDmaBroadcastBase<AIRSpecializeDmaBroadcast> {

public:
  AIRSpecializeDmaBroadcast() = default;
  AIRSpecializeDmaBroadcast(const AIRSpecializeDmaBroadcast &pass){};

  void runOnOperation() override {
    auto module = getOperation();
    SmallVector<func::FuncOp, 4> funcOps;
    module.walk([&](func::FuncOp op) { funcOps.push_back(op); });
    for (auto f : funcOps)
      runOnFunction(f);
  }

  void runOnFunction(func::FuncOp f) {
    // Specialize broadcastable DMA into affine.if regions
    specializeDmaBroadcastWithAffineIf(f);
    // Walk the affine.if's affine.set and simplify DMA source indices
    simplifyDmaIndicesWithAffineSet(f);
  }

private:

  void specializeDmaBroadcastWithAffineIf(func::FuncOp f) {

    f.walk([&](xilinx::air::HerdLaunchOp launch) {
      launch.walk([&](xilinx::air::DmaMemcpyInterface memcpyOp) {
        auto herd_id = launch.getTileIds();
        OpBuilder builder(memcpyOp);
        auto loc = memcpyOp->getLoc();
        auto broadcast_pattern = memcpyOp->getAttrOfType<mlir::IntegerSetAttr>("broadcast_pattern");
        auto ctx = memcpyOp->getContext();
        if (broadcast_pattern){
          auto is = broadcast_pattern.getValue();
          auto constraints = is.getConstraints();
          auto eqFlags = is.getEqFlags();

          unsigned numPartitions = 0;
          // Get symbol range (i.e. partition range)
          SmallVector<AffineExpr, 1> zero_syms{
              getAffineConstantExpr(0, ctx),
          };
          for (auto c : constraints) {
            if (c.isSymbolicOrConstant()){
              auto newC = c.replaceSymbols(zero_syms);
              auto expr = simplifyAffineExpr(newC, 0, 1).dyn_cast<AffineConstantExpr>();
              if (!expr){
                continue;
              }
              if (expr.getValue() != 0){
                numPartitions = expr.getValue() + 1;
              }
            }
          }
          // Walk each partition set in the patition scheme
          // Specialize affine set per partition
          for (unsigned i = 0; i < numPartitions; i++){
            SmallVector<AffineExpr, 2> newConstraints;
            SmallVector<bool, 2> newEqflags;
            SmallVector<AffineExpr, 1> i_syms{
                getAffineConstantExpr(i, ctx),
            };
            SmallVector<AffineExpr, 2> syms{
                getAffineSymbolExpr(0, ctx),
                getAffineSymbolExpr(1, ctx),
            };
            int c_iter = 0;
            for (auto c : constraints) {
              if (!c.isSymbolicOrConstant()){
                // Substitute partition id i_syms into inequalities
                auto newC = c.replaceSymbols(i_syms);
                // Replace all dims with symbols
                newC = newC.replaceDims(syms);
                newConstraints.push_back(newC);
                newEqflags.push_back(eqFlags[c_iter]);
              }
              c_iter++;
            }
            auto int_set = IntegerSet::get(0, 2, newConstraints, newEqflags);
            SmallVector<Value, 2> int_set_args{herd_id.x, herd_id.y};
            // Duplicate dma ops per spatial partition
            if (i == 0){
              AffineIfOp aif = builder.create<AffineIfOp>(loc, 
                      xilinx::air::AsyncTokenType::get(ctx), int_set,
                      int_set_args, (i != numPartitions - 1));
              builder.setInsertionPointToStart(aif.getThenBlock());
              auto memcpyOp_cloned = builder.clone(*memcpyOp.getOperation());
              memcpyOp_cloned->removeAttr("broadcast_pattern");
              memcpyOp_cloned->setAttr("broadcast_set",
                      mlir::IntegerSetAttr::get(int_set));
              SmallVector<Value, 1> yield_token;
              yield_token.push_back(dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp_cloned).getAsyncToken());
              builder.create<AffineYieldOp>(memcpyOp_cloned->getLoc(), yield_token);
              if (numPartitions != 1){
                // If more than 1 spatial partitions, then move loc to else block
                builder.setInsertionPointToStart(aif.getElseBlock());
              }
              // Reconnect dependency graph using the outermost affine.if's token
              auto async_memcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp.getOperation());
              async_memcpyOp.getAsyncToken().replaceAllUsesWith(aif.getResult(0));
            }
            else if (i < numPartitions - 1) {
              AffineIfOp aif = builder.create<AffineIfOp>(builder.getUnknownLoc(), 
                      xilinx::air::AsyncTokenType::get(ctx), int_set,
                      int_set_args, (i != numPartitions - 1));
              builder.setInsertionPointToStart(aif.getThenBlock());
              auto memcpyOp_cloned = builder.clone(*memcpyOp.getOperation());
              memcpyOp_cloned->removeAttr("broadcast_pattern");
              memcpyOp_cloned->setAttr("broadcast_set",
                      mlir::IntegerSetAttr::get(int_set));
              SmallVector<Value, 1> yield_token;
              yield_token.push_back(dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp_cloned).getAsyncToken());
              builder.create<AffineYieldOp>(memcpyOp_cloned->getLoc(), yield_token);
              builder.setInsertionPointAfter(aif);
              SmallVector<Value, 1> parent_block_yield_token = {aif.getResult(0)};
              builder.create<AffineYieldOp>(builder.getUnknownLoc(), parent_block_yield_token);
              builder.setInsertionPointToStart(aif.getElseBlock());
            }
            else {
              auto memcpyOp_cloned = builder.clone(*memcpyOp.getOperation());
              memcpyOp_cloned->removeAttr("broadcast_pattern");
              memcpyOp_cloned->setAttr("broadcast_set",
                      mlir::IntegerSetAttr::get(int_set));
              SmallVector<Value, 1> yield_token;
              yield_token.push_back(dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp_cloned).getAsyncToken());
              builder.create<AffineYieldOp>(memcpyOp_cloned->getLoc(), yield_token);
            }
          }
          memcpyOp.erase();
        }
      });
    });
  }

  void simplifyDmaIndicesWithAffineSet(func::FuncOp f) {

    f.walk([&](xilinx::air::DmaMemcpyInterface memcpyOp) {
      auto ctx = memcpyOp->getContext();
      if (auto broadcast_set = memcpyOp->getAttrOfType<mlir::IntegerSetAttr>("broadcast_set")){
        // Get all ops on the dependency connection between dma and herd launch
        SmallVector<Value, 1> loop_dep_history;
        std::vector<Operation *> op_history;
        traceDependentInductionVar(memcpyOp, loop_dep_history, op_history);

        // Walk constraints in broadcast pattern, and get shape of the broadcast pattern
        auto is = broadcast_set.getValue();
        auto constraints = is.getConstraints();
        auto eqFlags = is.getEqFlags();

        // Check which dimension op operates on; initialize current_shape_expr
        SmallVector<AffineExpr, 2> current_shape_expr = {nullptr, nullptr};
        for (auto v : loop_dep_history){
          if (auto hl_op = xilinx::air::getHerdLaunchTileIdOwner(v)){
            if (v == hl_op.getTileIds().x){
              unsigned dim = 0;
              for (unsigned i = 0; i < constraints.size(); i++){
                auto c = constraints[i];
                if (c.isFunctionOfSymbol(dim) && eqFlags[i]){
                  auto eval = evaluateSymbolEqualityInSet(c, ctx);
                  current_shape_expr[0] = getAffineConstantExpr(eval, ctx);
                }
              }
            }
            if (v == hl_op.getTileIds().y){
              unsigned dim = 1;
              for (unsigned i = 0; i < constraints.size(); i++){
                auto c = constraints[i];
                if (c.isFunctionOfSymbol(dim) && eqFlags[i]){
                  auto eval = evaluateSymbolEqualityInSet(c, ctx);
                  current_shape_expr[1] = getAffineConstantExpr(eval, ctx);
                }
              }
            }
          }
        }

        // Evaluate broadcast pattern by propagating expr through scalar operations in op history, last-in-first-out
        for (std::vector<Operation *>::reverse_iterator i = op_history.rbegin(); i != op_history.rend(); ++i ) {
          if (auto air_region_op = dyn_cast<xilinx::air::RegionOp>(*i)){
            assert(air_region_op.body().front().getOperations().size() == 2 
                    && "air::RegionOp should have only one child operation beside the terminator");
            // Get current scalar op
            Operation * op = nullptr;
            for (auto &child_op : air_region_op.body().front().getOperations()){
              if (!dyn_cast<xilinx::air::RegionTerminatorOp>(child_op)) op = &child_op;
            }
            // If the async op is affine.apply
            if (auto apply_op = dyn_cast<AffineApplyOp>(op)){
              auto map = apply_op.getAffineMap();
              if (current_shape_expr[0]){
                replaceSymbolAndEvaluateConstantInMap(map, current_shape_expr[0], ctx);
                // Remove dependence from scalar op to memcpyOp if present
                auto async_memcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp.getOperation());
                eraseAsyncDependencyFromAsyncOp(async_memcpyOp, air_region_op.getAsyncToken());
              }
              else if (current_shape_expr[1]){
                replaceSymbolAndEvaluateConstantInMap(map, current_shape_expr[1], ctx);
                // Remove dependence from scalar op to memcpyOp if present
                auto async_memcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp.getOperation());
                eraseAsyncDependencyFromAsyncOp(async_memcpyOp, air_region_op.getAsyncToken());
              }
            }

            // If the async op is arith op
            else if (auto arith_op = dyn_cast<arith::AddIOp>(op)){
              propagateAFfineConstantExprThroughArithOp<arith::AddIOp>(arith_op, current_shape_expr, memcpyOp.getOperation(), ctx);
            }
            else if (auto arith_op = dyn_cast<arith::MulIOp>(op)){
              propagateAFfineConstantExprThroughArithOp<arith::MulIOp>(arith_op, current_shape_expr, memcpyOp.getOperation(), ctx);
            }
          }
        } 

        // Replace memcpyOp's dependent operand with const
        auto newMemcpyOp = replaceMemcpyOpWithSimplifiedOperands(memcpyOp, current_shape_expr);
        auto asyncMemcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp.getOperation());
        auto asyncNewMemcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(newMemcpyOp);
        newMemcpyOp->setAttr("broadcast_set", broadcast_set);
        asyncMemcpyOp.getAsyncToken().replaceAllUsesWith(asyncNewMemcpyOp.getAsyncToken());
        memcpyOp->erase();
      }
    });
  }

  // Evaluate the integer value of affine set expression if the only symbolic identifier is replaced with zero
  int evaluateSymbolEqualityInSet(mlir::AffineExpr c, MLIRContext * ctx){
    assert(c.isSymbolicOrConstant() && "constraint has dimension identifier");
    SmallVector<AffineExpr, 2> zero_syms{
        getAffineConstantExpr(0, ctx),
        getAffineConstantExpr(0, ctx),
    };
    auto newC = c.replaceSymbols(zero_syms);
    auto expr = simplifyAffineExpr(newC, 0, 1).dyn_cast<AffineConstantExpr>();
    assert(expr);
    int result = expr.getValue();
    // Both + and - constant eval are legal for AffineExpr
    return (result >= 0) ? (result) : (-result);
  }

  // Evaluate the affine expression of affine map if the only symbolic identifier is replaced with zero
  void replaceSymbolAndEvaluateConstantInMap(mlir::AffineMap map, mlir::AffineExpr &c, MLIRContext * ctx){
    auto newmap = map.replace(getAffineSymbolExpr(0, ctx), c, 0, 1);
    auto const_int = simplifyAffineMap(newmap).getSingleConstantResult();
    c = getAffineConstantExpr(const_int, ctx);
  }

  // AddI for AffineConstantExpr
  void applyArithOpToAffineConstantExpr(arith::AddIOp arith_op, mlir::AffineExpr &c, MLIRContext * ctx){
    arith::ConstantIndexOp add_operand = nullptr;
    if (arith_op.getLhs().getDefiningOp() && dyn_cast<arith::ConstantIndexOp>(arith_op.getLhs().getDefiningOp())){
      add_operand = dyn_cast<arith::ConstantIndexOp>(arith_op.getLhs().getDefiningOp());
    }
    else if (arith_op.getRhs().getDefiningOp() && dyn_cast<arith::ConstantIndexOp>(arith_op.getRhs().getDefiningOp())){
      add_operand = dyn_cast<arith::ConstantIndexOp>(arith_op.getRhs().getDefiningOp());
    }
    else assert(false && "arith::AddIOp has no arith::ConstantIndexOp operand");
    auto acc = add_operand.value();
    assert(c.dyn_cast<mlir::AffineConstantExpr>() && "non-constant affine expression");
    acc += c.dyn_cast<mlir::AffineConstantExpr>().getValue();
    c = getAffineConstantExpr(acc, ctx);
  }

  // MulI for AffineConstantExpr
  void applyArithOpToAffineConstantExpr(arith::MulIOp arith_op, mlir::AffineExpr &c, MLIRContext * ctx){
    arith::ConstantIndexOp mul_operand = nullptr;
    if (arith_op.getLhs().getDefiningOp() && dyn_cast<arith::ConstantIndexOp>(arith_op.getLhs().getDefiningOp())){
      mul_operand = dyn_cast<arith::ConstantIndexOp>(arith_op.getLhs().getDefiningOp());
    }
    else if (arith_op.getRhs().getDefiningOp() && dyn_cast<arith::ConstantIndexOp>(arith_op.getRhs().getDefiningOp())){
      mul_operand = dyn_cast<arith::ConstantIndexOp>(arith_op.getRhs().getDefiningOp());
    }
    else assert(false && "arith::MulIOp has no arith::ConstantIndexOp operand");
    auto mul = mul_operand.value();
    assert(c.dyn_cast<mlir::AffineConstantExpr>() && "non-constant affine expression");
    mul *= c.dyn_cast<mlir::AffineConstantExpr>().getValue();
    c = getAffineConstantExpr(mul, ctx);
  }

  // Propagate AffineConstantExpr through arith addi/muli op
  template <typename T>
  void propagateAFfineConstantExprThroughArithOp(T arith_op, SmallVector<AffineExpr, 2> &current_shape_expr, Operation * memcpyOp, MLIRContext * ctx){
    xilinx::air::RegionOp parent_region_op = arith_op->template getParentOfType<xilinx::air::RegionOp>();
    if (current_shape_expr[0]){
      applyArithOpToAffineConstantExpr(arith_op, current_shape_expr[0], ctx);
      // Remove dependence from scalar op to memcpyOp if present
      auto async_memcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp);
      eraseAsyncDependencyFromAsyncOp(async_memcpyOp, parent_region_op.getAsyncToken());
    }
    else if (current_shape_expr[1]){
      applyArithOpToAffineConstantExpr(arith_op, current_shape_expr[1], ctx);
      // Remove dependence from scalar op to memcpyOp if present
      auto async_memcpyOp = dyn_cast<xilinx::air::AsyncOpInterface>(memcpyOp);
      eraseAsyncDependencyFromAsyncOp(async_memcpyOp, parent_region_op.getAsyncToken());
    }
  }

  // Replace memcpyOp's dependent operand with const
  Operation * replaceMemcpyOpWithSimplifiedOperands(xilinx::air::DmaMemcpyInterface &memcpyOp, SmallVector<AffineExpr, 2> current_shape_expr){
    OpBuilder builder(memcpyOp);
    builder.setInsertionPoint(memcpyOp);
    auto loc = memcpyOp->getLoc();
    SmallVector<Value, 1> srcMemrefDimsOrOffsets;
    if (auto memcpyNdOp = dyn_cast<xilinx::air::DmaMemcpyNdOp>(memcpyOp.getOperation())){
      for (unsigned i = 0; i < current_shape_expr.size(); i++){
        if (current_shape_expr[i]){
          auto val = current_shape_expr[i].template dyn_cast<AffineConstantExpr>().getValue();
          auto cop = builder.create<arith::ConstantIndexOp>(loc, val);
          srcMemrefDimsOrOffsets.push_back(cop);
        }
        else {
          srcMemrefDimsOrOffsets.push_back(memcpyNdOp.getSrcOffsets()[i]);
        }
      }  
      // Replace memcpyOp
      return replaceMemcpyOp(memcpyNdOp, builder, srcMemrefDimsOrOffsets);
    }
    else {
      for (unsigned i = 0; i < current_shape_expr.size(); i++){
        if (current_shape_expr[i]){
          auto val = current_shape_expr[i].template dyn_cast<AffineConstantExpr>().getValue();
          auto cop = builder.create<arith::ConstantIndexOp>(loc, val);
          srcMemrefDimsOrOffsets.push_back(cop);
        }
        else {
          srcMemrefDimsOrOffsets.push_back(memcpyOp.getSrcMemrefDim(i));
        }
      }
      // Replace memcpyOp
      if (auto op = dyn_cast<xilinx::air::DmaMemcpyOp>(memcpyOp.getOperation()))
        return replaceMemcpyOp(op, builder, srcMemrefDimsOrOffsets);
      else if (auto op = dyn_cast<xilinx::air::DmaMemcpy2dOp>(memcpyOp.getOperation()))
        return replaceMemcpyOp(op, builder, srcMemrefDimsOrOffsets);
      else if (auto op = dyn_cast<xilinx::air::DmaMemcpy4dOp>(memcpyOp.getOperation()))
        return replaceMemcpyOp(op, builder, srcMemrefDimsOrOffsets);
      else return nullptr;
    }

  }

  // Replace DmaMemcpyNdOp with updated src operands
  Operation * replaceMemcpyOp (xilinx::air::DmaMemcpyNdOp op, OpBuilder &builder, SmallVector<Value, 1> srcMemrefDimsOrOffsets){
    auto loc = op->getLoc();
    xilinx::air::DmaMemcpyNdOp newMemcpyOp = builder.create<xilinx::air::DmaMemcpyNdOp>(loc, xilinx::air::AsyncTokenType::get(op->getContext()), 
            op.getAsyncDependencies(), op.getDstMemref(), op.getDstOffsets(), op.getDstSizes(), op.getDstStrides(), op.getSrcMemref(), 
            srcMemrefDimsOrOffsets, op.getSrcSizes(), op.getSrcStrides()); 
    return newMemcpyOp.getOperation();
  }

  // Replace DmaMemcpyOp with updated src operands
  Operation * replaceMemcpyOp (xilinx::air::DmaMemcpyOp op, OpBuilder &builder, SmallVector<Value, 1> srcMemrefDimsOrOffsets){
    auto loc = op->getLoc();
    xilinx::air::DmaMemcpyOp newMemcpyOp = builder.create<xilinx::air::DmaMemcpyOp>(loc, xilinx::air::AsyncTokenType::get(op->getContext()), 
            op.getAsyncDependencies(), op.getDstMemref(), op.getSrcMemref(), op.getDstMemrefDim(0), srcMemrefDimsOrOffsets[0], op.getLength()); 
    return newMemcpyOp.getOperation();
  }

  // Replace DmaMemcpy2dOp with updated src operands
  Operation * replaceMemcpyOp (xilinx::air::DmaMemcpy2dOp op, OpBuilder &builder, SmallVector<Value, 1> srcMemrefDimsOrOffsets){
    auto loc = op->getLoc();
    xilinx::air::DmaMemcpy2dOp newMemcpyOp = builder.create<xilinx::air::DmaMemcpy2dOp>(loc, xilinx::air::AsyncTokenType::get(op->getContext()), 
            op.getAsyncDependencies(), op.getDstMemref(), op.getSrcMemref(), op.getDstMemrefDim(0), op.getDstMemrefDim(1), 
            srcMemrefDimsOrOffsets[0], srcMemrefDimsOrOffsets[1], op.getLength(), op.getStride(), op.getElemPerStride()); 
    return newMemcpyOp.getOperation();
  }

  // Replace DmaMemcpy4dOp with updated src operands
  Operation * replaceMemcpyOp (xilinx::air::DmaMemcpy4dOp op, OpBuilder &builder, SmallVector<Value, 1> srcMemrefDimsOrOffsets){
    auto loc = op->getLoc();
    xilinx::air::DmaMemcpy4dOp newMemcpyOp = builder.create<xilinx::air::DmaMemcpy4dOp>(loc, xilinx::air::AsyncTokenType::get(op->getContext()), 
            op.getAsyncDependencies(), op.getDstMemref(), op.getSrcMemref(), op.getDstMemrefDim(0), op.getDstMemrefDim(1), op.getDstMemrefDim(2), 
            op.getDstMemrefDim(3), srcMemrefDimsOrOffsets[0], srcMemrefDimsOrOffsets[1], srcMemrefDimsOrOffsets[2], srcMemrefDimsOrOffsets[3], 
            op.getLength(), op.getStride(), op.getElemPerStride()); 
    return newMemcpyOp.getOperation();
  }

};

class AIRFuseParallelHerdLaunchPass
    : public xilinx::air::AIRFuseParallelHerdLaunchPassBase<
          AIRFuseParallelHerdLaunchPass> {

public:
  AIRFuseParallelHerdLaunchPass() = default;
  AIRFuseParallelHerdLaunchPass(const AIRFuseParallelHerdLaunchPass &pass){};

  void runOnOperation() override;

private:
};

void AIRFuseParallelHerdLaunchPass::runOnOperation() {

  auto module = getOperation();
  // auto ctx = module.getContext();

  xilinx::air::HerdLaunchOp launchOp = nullptr;
  scf::ParallelOp parOp = nullptr;

  module.walk([&](xilinx::air::HerdLaunchOp launch) {
    // launch must be enclosed by scf.parallel
    parOp = launch->getParentOfType<scf::ParallelOp>();
    if (!parOp)
      return;

    // launch must be at the top level of the scf.parallel
    if (parOp.getBody() != launch->getBlock())
      return;

    launchOp = launch;
  });

  if (!launchOp || !parOp)
    return;

  // if the herd launch is size 1 in one dimension
  // and the herd launch is enclosed by a 1-d scf.parallel
  // then we try to fuse the scf.parallel onto the herd launch
  auto herd_size = launchOp.getHerdSizeOperands();
  int64_t herd_size_x =
      cast<arith::ConstantIndexOp>(herd_size.x.getDefiningOp()).value();
  int64_t herd_size_y =
      cast<arith::ConstantIndexOp>(herd_size.y.getDefiningOp()).value();
  if (herd_size_x != 1 && herd_size_y != 1)
    return;

  OpBuilder b(parOp);
  xilinx::air::HerdDim2 dims;
  if (herd_size_x == 1)
    dims = {parOp.getUpperBound()[0], herd_size.y};
  else
    dims = {herd_size.x, parOp.getUpperBound()[0]};

  SmallVector<Value, 8> args;
  SmallVector<Value, 4> constants;
  llvm::SetVector<Value> region_args;

  getUsedValuesDefinedAbove(parOp.getRegion(), region_args);
  for (Value v : region_args) {
    if (v.getDefiningOp() && isa<arith::ConstantOp>(v.getDefiningOp()))
      constants.push_back(v);
    else
      args.push_back(v);
  }

  auto newLaunchOp =
      b.create<xilinx::air::HerdLaunchOp>(parOp.getLoc(), dims, args);

  BlockAndValueMapping remap;
  remap.map(parOp.getInductionVars()[0], (herd_size_x == 1)
                                             ? newLaunchOp.getTileIds().x
                                             : newLaunchOp.getTileIds().y);

  b.setInsertionPointToStart(&newLaunchOp.body().front());

  for (auto &o : *parOp.getBody()) {
    if (isa<xilinx::air::HerdLaunchOp>(o)) {
      int idx = 0;
      remap.map(launchOp.getHerdSize().x, launchOp.getHerdSizeOperands().x);
      remap.map(launchOp.getHerdSize().y, launchOp.getHerdSizeOperands().y);
      remap.map(launchOp.getTileIds().x, (herd_size_x == 1)
                                             ? launchOp.getHerdSizeOperands().x
                                             : newLaunchOp.getTileIds().x);
      remap.map(launchOp.getTileIds().y,
                (herd_size_x == 1) ? newLaunchOp.getTileIds().y
                                   : launchOp.getHerdSizeOperands().y);
      for (auto &a : launchOp.getKernelArguments()) {
        auto v = launchOp.getKernelOperand(idx++);
        remap.map(a, remap.lookupOrDefault(v));
      }
      for (auto &ho : launchOp.body().front()) {
        if (isa<xilinx::air::HerdTerminatorOp>(ho))
          continue;
        b.clone(ho, remap);
      }
    } else if (isa<scf::YieldOp>(o)) {
      continue;
    } else {
      b.clone(o, remap);
    }
  }
  b.create<xilinx::air::HerdTerminatorOp>(parOp.getLoc());

  b.setInsertionPointToStart(&newLaunchOp.body().front());
  for (auto c : constants) {
    replaceAllUsesInRegionWith(c, b.clone(*c.getDefiningOp())->getResult(0),
                               newLaunchOp.getRegion());
  }

  int idx = 0;
  auto kernel_args = newLaunchOp.getKernelArguments();
  for (Value v : args)
    replaceAllUsesInRegionWith(v, kernel_args[idx++], newLaunchOp.getRegion());

  parOp.erase();
}

} // anonymous namespace

namespace xilinx {
namespace air {

std::unique_ptr<Pass> createAIRExamplePass() {
  return std::make_unique<AIRExamplePass>();
}

std::unique_ptr<Pass> createAIRSpecializeDma() {
  return std::make_unique<AIRSpecializeDma>();
}

std::unique_ptr<Pass> createAIRSpecializeDmaBroadcast() {
  return std::make_unique<AIRSpecializeDmaBroadcast>();
}

std::unique_ptr<Pass> createAIRPromoteUniformL1Dma() {
  return std::make_unique<AIRPromoteUniformL1Dma>();
}

std::unique_ptr<Pass> createAIRLinalgNamePass() {
  return std::make_unique<AIRLinalgNamePass>();
}

std::unique_ptr<Pass> createAIRRemoveLinalgNamePass() {
  return std::make_unique<AIRRemoveLinalgNamePass>();
}

std::unique_ptr<Pass> createAIRFuseParallelHerdLaunchPass() {
  return std::make_unique<AIRFuseParallelHerdLaunchPass>();
}

} // namespace air
} // namespace xilinx