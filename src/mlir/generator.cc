// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include "generator.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Types.h"
#include "utils.h"

#include <string>

/// LLVM aliases
using StructType = mlir::LLVM::LLVMStructType;
using PointerType = mlir::LLVM::LLVMPointerType;
using ArrayType = mlir::LLVM::LLVMArrayType;

namespace mlir::verona
{
  Value MLIRGenerator::typeConversion(Value val, Type ty)
  {
    auto valTy = val.getType();
    auto valSize = valTy.getIntOrFloatBitWidth();
    auto tySize = ty.getIntOrFloatBitWidth();
    if (valSize == tySize)
      return val;

    // Integer upcasts
    // TODO: Consiger sign, too
    auto valInt = valTy.dyn_cast<IntegerType>();
    auto tyInt = ty.dyn_cast<IntegerType>();
    if (valInt && tyInt)
    {
      if (valSize < tySize)
        return builder.create<SignExtendIOp>(val.getLoc(), ty, val);
      else
        return builder.create<TruncateIOp>(val.getLoc(), ty, val);
    }

    // Floating point casts
    auto valFP = valTy.dyn_cast<FloatType>();
    auto tyFP = ty.dyn_cast<FloatType>();
    if (valFP && tyFP)
    {
      if (valSize < tySize)
        return builder.create<FPExtOp>(val.getLoc(), ty, val);
      else
        return builder.create<FPTruncOp>(val.getLoc(), ty, val);
    }

    // If not compatible, assert
    assert(false && "Type casts between incompatible types");

    // Appease MSVC warnings
    return Value();
  }

  std::pair<mlir::Value, mlir::Value>
  MLIRGenerator::typePromotion(mlir::Value lhs, mlir::Value rhs)
  {
    auto lhsType = lhs.getType();
    auto rhsType = rhs.getType();

    // Shortcut for when both are the same
    if (lhsType == rhsType)
      return {lhs, rhs};

    auto lhsSize = lhsType.getIntOrFloatBitWidth();
    auto rhsSize = rhsType.getIntOrFloatBitWidth();

    // Promote the smallest to the largest
    if (lhsSize < rhsSize)
      lhs = typeConversion(lhs, rhsType);
    else
      rhs = typeConversion(rhs, lhsType);

    return {lhs, rhs};
  }

  llvm::Expected<FuncOp> MLIRGenerator::generateProto(
    Location loc,
    llvm::StringRef name,
    llvm::ArrayRef<Type> types,
    llvm::ArrayRef<Type> retTy)
  {
    // Create function
    auto funcTy = builder.getFunctionType(types, {retTy});
    auto func = FuncOp::create(loc, name, funcTy);
    // FIXME: This should be private unless we export, but for now we make
    // it public to test IR generation before implementing public visibility
    func.setVisibility(SymbolTable::Visibility::Public);
    return func;
  }

  llvm::Expected<FuncOp> MLIRGenerator::generateEmptyFunction(
    Location loc,
    llvm::StringRef name,
    llvm::ArrayRef<Type> types,
    llvm::ArrayRef<Type> retTy)
  {
    // If it's not declared yet, do so. This simplifies direct declaration
    // of compiler functions. User functions should be checked at the parse
    // level.
    auto func = module->lookupSymbol<FuncOp>(name);
    if (!func)
    {
      auto proto = generateProto(loc, name, types, retTy);
      if (auto err = proto.takeError())
        return std::move(err);
      func = *proto;
    }

    // Create entry block, set builder entry point
    auto& entryBlock = *func.addEntryBlock();
    builder.setInsertionPointToStart(&entryBlock);

    return func;
  }

  llvm::Expected<Value> MLIRGenerator::generateCall(
    Location loc, FuncOp func, llvm::ArrayRef<Value> args)
  {
    // TODO: Implement dynamic method calls
    auto call = builder.create<CallOp>(loc, func, args);
    // TODO: Implement multiple return values (tuples?)
    return call->getOpResult(0);
  }

  llvm::Expected<Value> MLIRGenerator::generateArithmetic(
    Location loc, llvm::StringRef opName, Value lhs, Value rhs)
  {
    // FIXME: Implement all unary and binary operators
    assert(lhs && rhs && "No binary operation with less than two arguments");

    // Make sure we're dealing with values, not pointers
    // FIXME: This shouldn't be necessary at this point
    if (isPointer(lhs))
      lhs = generateLoad(loc, lhs);
    if (isPointer(rhs))
      rhs = generateLoad(loc, rhs);

    // Promote types to be the same, or ops don't work, in the end, both
    // types are identical and the same as the return type.
    std::tie(lhs, rhs) = typePromotion(lhs, rhs);
    auto retTy = lhs.getType();

    // FIXME: We already converted U32 to i32 so this "works". But we need
    // to make sure we want that conversion as early as it is, and if not,
    // we need to implement this as a standard select and convert that
    // later. However, that would only work if U32 has a method named "+",
    // or if we declare it on the fly and then clean up when we remove the
    // call.

    // Floating point arithmetic
    if (retTy.isF32() || retTy.isF64())
    {
      auto op = llvm::StringSwitch<Value>(opName)
                  .Case("+", builder.create<AddFOp>(loc, retTy, lhs, rhs))
                  .Default({});
      assert(op && "Unknown arithmetic operator");
      return op;
    }

    // Integer arithmetic
    assert(retTy.isa<IntegerType>() && "Bad arithmetic types");
    auto op = llvm::StringSwitch<Value>(opName)
                .Case("+", builder.create<AddIOp>(loc, retTy, lhs, rhs))
                .Default({});
    assert(op && "Unknown arithmetic operator");
    return op;
  }

  Value MLIRGenerator::generateAlloca(Location loc, Type ty)
  {
    PointerType pointerTy;
    Value len = generateConstant(builder.getI32Type(), 1);
    pointerTy = PointerType::get(ty);
    return builder.create<LLVM::AllocaOp>(loc, pointerTy, len);
  }

  Value MLIRGenerator::generateGEP(Location loc, Value addr, int offset)
  {
    llvm::SmallVector<Value> offsetList;
    // First argument is always in context of a list
    if (isStructPointer(addr))
    {
      auto zero = generateZero(builder.getI32Type());
      offsetList.push_back(zero);
    }
    // Second argument is in context of the struct
    auto len = generateConstant(builder.getI32Type(), offset);
    offsetList.push_back(len);
    ValueRange index(offsetList);
    Type retTy = addr.getType();
    if (auto structTy = getElementType(addr).dyn_cast<StructType>())
      retTy = getFieldType(structTy, offset);
    return builder.create<LLVM::GEPOp>(loc, retTy, addr, index);
  }

  Value MLIRGenerator::generateLoad(Location loc, Value addr, int offset)
  {
    if (!isa<LLVM::GEPOp>(addr.getDefiningOp()))
      addr = generateGEP(loc, addr, offset);
    else
      assert(offset == 0 && "Can't take an offset of a GEP");
    return builder.create<LLVM::LoadOp>(loc, addr);
  }

  Value
  MLIRGenerator::generateAutoLoad(Location loc, Value addr, Type ty, int offset)
  {
    // If it's not an address, there's nothing to load
    if (!isPointer(addr))
      return addr;

    // If the expected type is a pointer, we want the address, not the value
    if (ty && ty.isa<PointerType>())
      return addr;

    auto elmTy = getElementType(addr);

    // If type was specified, check it matches the address type
    if (ty)
      assert(elmTy == ty && "Invalid pointer load");

    return generateLoad(loc, addr, offset);
  }

  void
  MLIRGenerator::generateStore(Location loc, Value addr, Value val, int offset)
  {
    if (!isa<LLVM::GEPOp>(addr.getDefiningOp()))
      addr = generateGEP(loc, addr, offset);
    else
      assert(offset == 0 && "Can't take an offset of a GEP");
    builder.create<LLVM::StoreOp>(loc, val, addr);
  }

  Value MLIRGenerator::generateConstant(Type ty, std::variant<int, double> val)
  {
    auto loc = builder.getUnknownLoc();
    if (ty.isIndex())
    {
      return builder.create<ConstantIndexOp>(loc, std::get<int>(val));
    }
    else if (auto it = ty.dyn_cast<IntegerType>())
    {
      return builder.create<ConstantIntOp>(loc, std::get<int>(val), it);
    }
    else if (auto ft = ty.dyn_cast<FloatType>())
    {
      APFloat value = APFloat(std::get<double>(val));
      return builder.create<ConstantFloatOp>(loc, value, ft);
    }

    assert(0 && "Type not supported for zero");

    // Return invalid value for release builds
    // FIXME: Attach diagnostics engine here to report problems like these.
    return Value();
  }

  Value MLIRGenerator::generateZero(Type ty)
  {
    if (ty.isa<FloatType>())
      return generateConstant(ty, 0.0);
    else
      return generateConstant(ty, 0);
  }

  Value MLIRGenerator::generateConstantString(StringRef str, StringRef name)
  {
    // Use auto-generated name if none provided
    static size_t incr = 0;
    std::string nameStr;
    if (name.empty())
      nameStr = "_string" + std::to_string(incr++);
    else
      nameStr = name.str();

    // In LLVM, strings are arrays of i8 elements
    auto i8 = builder.getIntegerType(8);
    auto strTy = ArrayType::get(i8, str.size());
    auto strAttr = builder.getStringAttr(str);

    // In LLVM, constant strings are global objects
    auto moduleBuilder = OpBuilder(*module);
    auto global = moduleBuilder.create<LLVM::GlobalOp>(
      builder.getUnknownLoc(),
      strTy,
      /*isConstant=*/true,
      LLVM::Linkage::Private,
      nameStr,
      strAttr);
    module->push_back(global);

    // But their addresses are a local operation
    auto addr =
      builder.create<LLVM::AddressOfOp>(builder.getUnknownLoc(), global);
    return addr->getResult(0);
  }
}
