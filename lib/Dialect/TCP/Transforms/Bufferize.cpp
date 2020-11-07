//===- Bufferize.cpp - Bufferization for TCP dialect -------------*- C++-*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Module.h"
#include "mlir/Transforms/Bufferize.h"
#include "mlir/Transforms/DialectConversion.h"
#include "npcomp/Dialect/Refback/IR/RefbackDialect.h"
#include "npcomp/Dialect/Refback/IR/RefbackOps.h"
#include "npcomp/Dialect/TCP/IR/TCPDialect.h"
#include "npcomp/Dialect/TCP/IR/TCPOps.h"
#include "npcomp/Dialect/TCP/Transforms/Passes.h"

using namespace mlir;
using namespace mlir::NPCOMP;

// TODO: Don't just open-code all shape transfer functions here.
static SmallVector<Value, 6> bypassResultShapes(Operation &op) {
  OpBuilder builder(&op);

  if (auto broadcastTo = dyn_cast<tcp::BroadcastToOp>(op)) {
    return {broadcastTo.shape()};
  }

  if (auto matmul = dyn_cast<tcp::MatmulOp>(op)) {
    auto lhsRows = builder.create<DimOp>(op.getLoc(), matmul.lhs(), 0);
    auto rhsCols = builder.create<DimOp>(op.getLoc(), matmul.rhs(), 1);
    auto shape = builder.create<TensorFromElementsOp>(
        op.getLoc(), ValueRange({lhsRows, rhsCols}));
    return {shape};
  }

  // No shape transfer function.
  return {};
}

static FailureOr<SmallVector<Value, 6>>
allocateResults(Operation *op, ConversionPatternRewriter &rewriter,
                Location loc,
                SmallVectorImpl<Value> *resultShapesOut = nullptr) {
  auto resultShapes = bypassResultShapes(*op);
  SmallVector<Value, 6> results;
  for (auto t : llvm::zip(op->getResults(), resultShapes)) {
    auto result = std::get<0>(t);
    auto resultShape = std::get<1>(t);
    auto tensorType = result.getType().cast<RankedTensorType>();
    auto memrefType =
        MemRefType::get(tensorType.getShape(), tensorType.getElementType());
    auto memref =
        rewriter.create<refback::AllocMemRefOp>(loc, memrefType, resultShape);
    results.push_back(memref);
  }
  if (resultShapesOut)
    resultShapesOut->append(resultShapes.begin(), resultShapes.end());
  return results;
}

namespace {
// TODO: Lower to a "buffer version" of tcp::BroadcastTo instead of directly to
// loops.
class LowerBroadcastToToLoopsPattern
    : public OpConversionPattern<tcp::BroadcastToOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tcp::BroadcastToOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto resultType = op.getType().cast<RankedTensorType>();
    auto inputType = op.operand().getType().cast<RankedTensorType>();
    SmallVector<Value, 6> resultShapes;
    auto resultsOrFailure =
        allocateResults(op, rewriter, op.getLoc(), &resultShapes);
    if (failed(resultsOrFailure))
      return failure();
    Value resultMemref = (*resultsOrFailure)[0];
    auto resultShape = resultShapes[0];
    Value inputMemref = operands[0];

    SmallVector<Value, 6> outputExtents;
    for (int i = 0, e = resultType.getRank(); i < e; i++) {
      Value dimIndex = rewriter.create<ConstantIndexOp>(op.getLoc(), i);
      Value outputExtent = rewriter.create<shape::GetExtentOp>(
          op.getLoc(), rewriter.getIndexType(), resultShape, dimIndex);
      outputExtents.push_back(outputExtent);
    }
    int rankDiff = resultType.getRank() - inputType.getRank();
    SmallVector<Value, 6> inputDimRequiresBroadcasting;
    for (int i = 0, e = inputType.getRank(); i < e; i++) {
      // Calculate the relevant extents.
      Value inputExtent = rewriter.create<DimOp>(op.getLoc(), op.operand(), i);
      inputDimRequiresBroadcasting.push_back(
          rewriter.create<CmpIOp>(op.getLoc(), CmpIPredicate::ne, inputExtent,
                                  outputExtents[rankDiff + i]));
    }

    {
      OpBuilder::InsertionGuard guard(rewriter);
      Value c0 = rewriter.create<ConstantIndexOp>(op.getLoc(), 0);
      Value c1 = rewriter.create<ConstantIndexOp>(op.getLoc(), 1);

      SmallVector<Value, 6> inductionVariables;
      // Create the (perfectly nested) loops.
      // Loop invariant: At the start of iteration `i`, the rewriter insertion
      // point is inside `i` nested loops.
      for (int i = 0, e = resultType.getRank(); i < e; i++) {
        auto loop = rewriter.create<scf::ForOp>(
            op.getLoc(), c0, outputExtents[i], c1, ValueRange({}));
        Block *body = loop.getBody();
        inductionVariables.push_back(body->getArgument(0));
        // Leave the insertion point at the beginning of the body.
        rewriter.setInsertionPointToStart(body);
      }

      // Create the inner loop body.
      // When reading from the input, clamp any indices for dimensions that are
      // being broadcast.
      SmallVector<Value, 6> inputIndices;
      for (int i = 0, e = inputType.getRank(); i < e; i++) {
        auto c0 = rewriter.create<ConstantIndexOp>(op.getLoc(), 0);
        auto select = rewriter.create<SelectOp>(
            op.getLoc(), inputDimRequiresBroadcasting[i], c0,
            inductionVariables[rankDiff + i]);
        inputIndices.push_back(select);
      }
      Value load =
          rewriter.create<LoadOp>(op.getLoc(), inputMemref, inputIndices);
      rewriter.create<StoreOp>(op.getLoc(), load, resultMemref,
                               inductionVariables);
    }
    rewriter.replaceOp(op, resultMemref);
    return success();
  }
};
} // namespace

namespace {
class BufferizeMatmulOp : public OpConversionPattern<tcp::MatmulOp> {
public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tcp::MatmulOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto resultsOrFailure = allocateResults(op, rewriter, op.getLoc());
    if (failed(resultsOrFailure))
      return failure();
    auto results = *resultsOrFailure;
    auto c0 =
        rewriter.create<ConstantOp>(op.getLoc(), rewriter.getF32FloatAttr(0.0));
    rewriter.create<linalg::FillOp>(op.getLoc(), results[0], c0);
    rewriter.create<linalg::MatmulOp>(op.getLoc(), operands, results);
    rewriter.replaceOp(op, results);
    return success();
  }
};
} // namespace

namespace {
class TCPBufferizePass : public TCPBufferizeBase<TCPBufferizePass> {
  void getDependentDialects(::mlir::DialectRegistry &registry) const override {
    registry.insert<refback::RefbackDialect>();
    registry.insert<linalg::LinalgDialect>();
    registry.insert<scf::SCFDialect>();
    registry.insert<shape::ShapeDialect>();
  }

  void runOnOperation() override {
    auto func = getOperation();
    auto *context = &getContext();

    BufferizeTypeConverter typeConverter;

    OwningRewritePatternList patterns;

    ConversionTarget target(*context);

    // All lowering to buffers involves refback.alloc_memref ops.
    // TODO: This makes the tests cleaner, but otherwise isn't too essential as
    // we can just open-code the extents for the alloc.
    target.addLegalOp<refback::AllocMemRefOp>();

    patterns.insert<LowerBroadcastToToLoopsPattern>(typeConverter, context);
    target.addIllegalOp<tcp::BroadcastToOp>();
    patterns.insert<BufferizeMatmulOp>(typeConverter, context);
    target.addIllegalOp<tcp::MatmulOp>();

    target.addLegalDialect<linalg::LinalgDialect>();
    target.addLegalDialect<StandardOpsDialect>();
    target.addLegalDialect<scf::SCFDialect>();
    target.addLegalOp<shape::GetExtentOp>();

    if (failed(applyPartialConversion(func, target, std::move(patterns))))
      return signalPassFailure();
  }
};
} // namespace

std::unique_ptr<OperationPass<FuncOp>> mlir::NPCOMP::createTCPBufferizePass() {
  return std::make_unique<TCPBufferizePass>();
}