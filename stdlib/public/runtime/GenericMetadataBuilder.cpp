//===--- GenericMetadataBuilder.cpp - Code to build generic metadata. -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Builder for generic metadata, in-process and out-of-process.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/GenericMetadataBuilder.h"
#include "MetadataCache.h"
#include "Private.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/TargetLayout.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/Metadata.h"
#include <string>
#include <type_traits>

#if SWIFT_STDLIB_HAS_DLADDR && __has_include(<dlfcn.h>)
#include <dlfcn.h>
#define USE_DLADDR 1
#endif

using namespace swift;

#define LOG(fmt, ...)                                                          \
  log(__FILE_NAME__, __LINE__, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)

/// A ReaderWriter (as used by GenericMetadataBuilder) that works in-process.
/// Pointer writing and pointer resolution are just raw pointer operations. Type
/// lookup is done by asking the runtime. Symbol lookup uses `dlsym`.
class InProcessReaderWriter {
public:
  using Runtime = InProcess;

  using Size = typename Runtime::StoredSize;
  using StoredPointer = typename Runtime::StoredPointer;
  using GenericArgument = void *;

  /// A typed buffer which wraps a value, or values, of type T.
  template <typename T>
  class Buffer {
  public:
    Buffer() : ptr(nullptr) {}

    Buffer(T *ptr) : ptr(ptr) {}

    /// Construct an arbitrarily typed buffer from a Buffer<const char>, using
    /// const char as an "untyped" buffer type.
    Buffer(Buffer<const char> buffer)
        : ptr(reinterpret_cast<T *>(buffer.ptr)) {}

    /// The pointer to the buffer's underlying storage.
    T *ptr;

    template <typename U>
    Buffer<U> cast() {
      return Buffer<U>(reinterpret_cast<U *>(ptr));
    }

    bool isNull() const { return !ptr; }

    /// The various resolvePointer functions take a pointer to a pointer within
    /// the buffer, and dereference it. In-process, this is a simple operation,
    /// basically just wrapping the * operator or get() function. This
    /// abstraction is needed for out-of-process operations.

    BuilderErrorOr<Buffer<char>> resolvePointer(uintptr_t *ptr) {
      return Buffer<char>{reinterpret_cast<char *>(*ptr)};
    }

    template <typename U, bool Nullable>
    BuilderErrorOr<Buffer<U>>
    resolvePointer(const RelativeDirectPointer<U, Nullable> *ptr) {
      return Buffer<U>{ptr->get()};
    }

    template <typename U, bool Nullable>
    BuilderErrorOr<Buffer<U>>
    resolvePointer(const RelativeIndirectablePointer<U, Nullable> *ptr) {
      return {ptr->get()};
    }

    template <typename U, bool Nullable>
    BuilderErrorOr<Buffer<const U>>
    resolvePointer(const RelativeIndirectablePointer<const U, Nullable> *ptr) {
      return Buffer<const U>{ptr->get()};
    }

    template <typename U>
    auto resolvePointer(const U *ptr)
        -> BuilderErrorOr<Buffer<std::remove_reference_t<decltype(**ptr)>>> {
      return Buffer<std::remove_reference_t<decltype(**ptr)>>{*ptr};
    }

    template <typename U>
    BuilderErrorOr<Buffer<const char>> resolveFunctionPointer(const U *ptr) {
      return Buffer<const char>{reinterpret_cast<const char *>(*ptr)};
    }

    template <typename U, bool nullable>
    BuilderErrorOr<Buffer<const char>> resolveFunctionPointer(
        TargetCompactFunctionPointer<Runtime, U, nullable> *ptr) {
      return Buffer<const char>{reinterpret_cast<const char *>(ptr->get())};
    }

    /// Get an address value for the buffer, for logging purposes.
    uint64_t getAddress() { return (uint64_t)ptr; }
  };

  /// WritableData is a mutable Buffer subclass.
  template <typename T>
  class WritableData : public Buffer<T> {
    /// Check that the given pointer lies within memory of this data object.
    void checkPtr(void *toCheck) {
      assert((uintptr_t)toCheck - (uintptr_t)this->ptr < size);
    }

  public:
    WritableData(T *ptr, size_t size) : Buffer<T>(ptr), size(size) {}

    size_t size;

    /// The various writePointer functions take a pointer to a pointer within
    /// the data, and a target, and set the pointer to the target. When done
    /// in-process, this is just a wrapper around the * and = operators. This
    /// abstracted is needed for out-of-process work.

    template <typename U>
    void writePointer(StoredPointer *to, Buffer<U> target) {
      checkPtr(to);
      *to = reinterpret_cast<StoredPointer>(target.ptr);
    }

    template <typename U>
    void writePointer(U **to, Buffer<U> target) {
      checkPtr(to);
      *to = target.ptr;
    }

    template <typename U>
    void writePointer(const U **to, Buffer<U> target) {
      checkPtr(to);
      *to = target.ptr;
    }

    void writePointer(const Metadata **to, GenericArgument target) {
      checkPtr(to);
      *to = reinterpret_cast<const Metadata *>(target);
    }

    template <typename To, typename From>
    void writePointer(To *to, Buffer<From> target) {
      checkPtr((void *)to);
      *to = target.ptr;
    }

    template <typename U>
    void writeFunctionPointer(U *to, Buffer<const char> target) {
      checkPtr((void *)to);
      // This weird double cast handles the case where the function pointer
      // type has a custom __ptrauth attribute, which the compiler doesn't like
      // casting to.
      auto castTarget = (const decltype(&**to))(void *)target.ptr;
      *to = castTarget;
    }
  };

  /// Basic info about a symbol.
  struct SymbolInfo {
    std::string symbolName;
    std::string libraryName;
    uint64_t pointerOffset;
  };

  /// Get info about the symbol corresponding to the given buffer. If no
  /// information can be retrieved, the result is filled with "<unknown>"
  /// strings and a 0 offset.
  template <typename T>
  SymbolInfo getSymbolInfo(Buffer<T> buffer) {
#if USE_DLADDR
    Dl_info info;
    int result = dladdr(buffer.ptr, &info);
    if (result == 0)
      return {"<unknown>", "<unknown>", 0};

    if (info.dli_fname == nullptr)
      info.dli_fname = "<unknown>";
    if (info.dli_sname == nullptr)
      info.dli_sname = "<unknown>";

    const char *libName = info.dli_fname;
    if (auto slash = strrchr(libName, '/'))
      libName = slash + 1;

    return {info.dli_sname, libName,
            buffer.getAddress() - (uintptr_t)info.dli_fbase};
#else
    return {"<unknown>", "<unknown>", 0};
#endif
  }

  /// Given a symbol name, retrieve a buffer pointing to the symbol's data.
  template <typename T = char>
  BuilderErrorOr<Buffer<const T>> getSymbolPointer(const char *name) {
#if USE_DLADDR
#ifdef RTLD_SELF
    // Use RTLD_SELF for performance where it's available.
    void *ptr = dlsym(RTLD_SELF, name);
#else
    // Otherwise use RTLD_DEFAULT to search everything.
    void *ptr = dlsym(RTLD_DEFAULT, name);
#endif
    LOG("getSymbolPointer(\"%s\") -> %p", name, ptr);
    if (!ptr)
      return BuilderError("dlsym could not find symbol '%s'", name);
    return Buffer<const T>{reinterpret_cast<const T *>(ptr)};
#else
    return BuilderError("getSymbolPointer is not implemented on this platform");
#endif
  }

  /// Look up a type with a given mangled name, in the context of the given
  /// metadata. The metadata's generic arguments must already be installed. Used
  /// for retrieving metadata for field records.
  BuilderErrorOr<Buffer<const Metadata>> getTypeByMangledName(
      WritableData<FullMetadata<Metadata>> containingMetadataBuffer,
      NodePointer metadataMangleNode, llvm::StringRef mangledTypeName) {
    auto metadata = static_cast<Metadata *>(containingMetadataBuffer.ptr);
    SubstGenericParametersFromMetadata substitutions(metadata);
    auto result = swift_getTypeByMangledName(
        MetadataState::LayoutComplete, mangledTypeName,
        substitutions.getGenericArgs(),
        [&substitutions, this](unsigned depth, unsigned index) {
          auto result = substitutions.getMetadata(depth, index).Ptr;
          LOG("substitutions.getMetadata(%u, %u).Ptr = %p", depth, index,
              result);
          return result;
        },
        [&substitutions, this](const Metadata *type, unsigned index) {
          auto result = substitutions.getWitnessTable(type, index);
          LOG("substitutions.getWitnessTable(%p, %u) = %p", type, index,
              result);
          return result;
        });
    if (result.isError()) {
      return *result.getError();
    }
    return Buffer<const Metadata>{result.getType().getMetadata()};
  }

  /// Allocate a WritableData with the given size.
  template <typename T>
  WritableData<T> allocate(size_t size) {
    auto bytes = reinterpret_cast<T *>(
        MetadataAllocator(MetadataAllocatorTags::GenericValueMetadataTag)
            .Allocate(size, alignof(void *)));

    return WritableData<T>{bytes, size};
  }

  bool isLoggingEnabled() { return true; }

  [[gnu::format(printf, 5, 6)]] void log(const char *filename, unsigned line,
                                         const char *function, const char *fmt,
                                         ...) {
    if (swift::runtime::environment::
            SWIFT_DEBUG_VALIDATE_EXTERNAL_GENERIC_METADATA_BUILDER() < 2)
      return;

    va_list args;
    va_start(args, fmt);

    fprintf(stderr, "%s:%u:%s: ", filename, line, function);
    vfprintf(stderr, fmt, args);
    fputs("\n", stderr);

    va_end(args);
  }
};

static BuilderErrorOr<ValueMetadata *> allocateGenericValueMetadata(
    const ValueTypeDescriptor *description, const void *arguments,
    const GenericValueMetadataPattern *pattern, size_t extraDataSize) {
  InProcessReaderWriter readerWriter;
  GenericMetadataBuilder builder{readerWriter};
  auto result = ERROR_CHECK(builder.buildGenericValueMetadata(
      {description},
      reinterpret_cast<const InProcessReaderWriter::GenericArgument *>(
          arguments),
      {pattern}, extraDataSize));

  char *base = reinterpret_cast<char *>(result.data.ptr);
  return reinterpret_cast<ValueMetadata *>(base + result.offset);
}

static bool initializeGenericValueMetadata(Metadata *metadata) {
  InProcessReaderWriter readerWriter;
  GenericMetadataBuilder builder{readerWriter};

  auto result = builder.initializeGenericMetadata(
      {asFullMetadata(metadata), -1u}, nullptr);
  if (auto *error = result.getError()) {
    fprintf(stderr, "swift_initializeGenericValueMetadata failed: %s",
            error->cStr());
    return false;
  }
  return true;
}

static BuilderErrorOr<size_t>
genericValueDataExtraSize(const ValueTypeDescriptor *description,
                          const GenericMetadataPattern *pattern) {
  InProcessReaderWriter readerWriter;
  GenericMetadataBuilder builder{readerWriter};
  return builder.extraDataSize({description}, {pattern});
}

[[gnu::format(printf, 2, 3)]] static void
validationLog(bool isValidationFailure, const char *fmt, ...) {
  if (!isValidationFailure &&
      swift::runtime::environment::
              SWIFT_DEBUG_VALIDATE_EXTERNAL_GENERIC_METADATA_BUILDER() < 2)
    return;
  FILE *output = stderr;

  va_list args;
  va_start(args, fmt);

  fputs("GenericMetadataBuilder validation: ", output);
  vfprintf(output, fmt, args);
  fputs("\n", output);

  va_end(args);
}

[[gnu::format(printf, 1, 2)]] static void printToStderr(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  vfprintf(stderr, fmt, args);

  va_end(args);
}

static BuilderErrorOr<std::monostate> dumpMetadata(const Metadata *metadata) {
  GenericMetadataBuilder<InProcessReaderWriter>::Dumper dumper(printToStderr);
  return dumper.dumpMetadata({metadata});
}

template <typename T>
static const T &unwrapVWTField(const T &field) {
  return field;
}

static uint32_t unwrapVWTField(const ValueWitnessFlags &field) {
  return field.getOpaqueValue();
}

static bool equalVWTs(const ValueWitnessTable *a, const ValueWitnessTable *b) {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define FUNCTION_VALUE_WITNESS(LOWER_ID, UPPER_ID, RET, PARAMS)                \
  if (a->LOWER_ID != b->LOWER_ID)                                              \
    return false;
#define VALUE_WITNESS(LOWER_ID, UPPER_ID)                                      \
  if (unwrapVWTField(a->LOWER_ID) != unwrapVWTField(b->LOWER_ID))              \
    return false;
#include "swift/ABI/ValueWitness.def"

  auto *enumA = dyn_cast<EnumValueWitnessTable>(a);
  auto *enumB = dyn_cast<EnumValueWitnessTable>(b);
  if (enumA == nullptr && enumB == nullptr) {
    return true;
  }
  if (enumA != nullptr && enumB != nullptr) {
#define WANT_ONLY_ENUM_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID)                                      \
  if (unwrapVWTField(enumA->LOWER_ID) != unwrapVWTField(enumB->LOWER_ID))      \
    return false;
#include "swift/ABI/ValueWitness.def"
    return true;
  }
  // Only one of a and b is an enum table.
  return false;
}

void swift::validateExternalGenericMetadataBuilder(
    const Metadata *original, const TypeContextDescriptor *description,
    const void *arguments) {
  if (auto valueDescriptor = dyn_cast<ValueTypeDescriptor>(description)) {
    if (valueDescriptor->isGeneric()) {
      auto pattern = reinterpret_cast<GenericValueMetadataPattern *>(
          valueDescriptor->getFullGenericContextHeader()
              .DefaultInstantiationPattern.get());
      auto extraDataSize = genericValueDataExtraSize(valueDescriptor, pattern);
      if (auto *error = extraDataSize.getError()) {
        validationLog(false, "error getting extra data size: %s",
                      error->cStr());
        return;
      }

      auto maybeNewMetadata = allocateGenericValueMetadata(
          valueDescriptor, arguments, pattern, *extraDataSize.getValue());
      if (auto *error = maybeNewMetadata.getError()) {
        validationLog(false, "error allocating metadata: %s", error->cStr());
        return;
      }
      auto *newMetadata = *maybeNewMetadata.getValue();
      bool success = initializeGenericValueMetadata(newMetadata);
      if (!success)
        return;

      auto origVWT = asFullMetadata(original)->ValueWitnesses;
      auto newVWT = asFullMetadata(newMetadata)->ValueWitnesses;

      bool equal = true;
      if (!equalVWTs(origVWT, newVWT)) {
        validationLog(true, "VWTs do not match");
        equal = false;
      }
      size_t totalSize = sizeof(ValueMetadata) + *extraDataSize.getValue();
      if (memcmp(original, newMetadata, totalSize)) {
        validationLog(true, "Metadatas do not match");
        equal = false;
      }

      if (!equal) {
        validationLog(true,
                      "Error! Mismatch between new/old metadata builders!");
        validationLog(true, "Original metadata:");
        if (auto *error = dumpMetadata(original).getError())
          validationLog(true, "error dumping original metadata: %s",
                        error->cStr());
        validationLog(true, "New metadata builder:");
        if (auto *error = dumpMetadata(newMetadata).getError())
          validationLog(true, "error dumping new metadata: %s", error->cStr());
        swift::fatalError(0, "Fatal error: mismatched metadata.\n");
      }

      auto typeName = swift_getTypeName(original, false);
      validationLog(false, "Validated generic metadata builder on %.*s",
                    (int)typeName.length, typeName.data);
    }
  }
}
