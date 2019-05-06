/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS bytecode descriptors, disassemblers, and (expression) decompilers.
 */

#include "vm/BytecodeUtil-inl.h"

#define __STDC_FORMAT_MACROS

#include "mozilla/Attributes.h"
#include "mozilla/ReverseIterator.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Vector.h"

#include <algorithm>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "jsapi.h"
#include "jsnum.h"
#include "jstypes.h"
#include "jsutil.h"

#include "builtin/String.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/SourceNotes.h"
#include "gc/FreeOp.h"
#include "gc/GCInternals.h"
#include "js/CharacterEncoding.h"
#include "js/Printf.h"
#include "js/Symbol.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/CodeCoverage.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/Realm.h"
#include "vm/Shape.h"

#include "gc/PrivateIterators-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Realm-inl.h"

using namespace js;

using js::frontend::IsIdentifier;

/*
 * Index limit must stay within 32 bits.
 */
JS_STATIC_ASSERT(sizeof(uint32_t) * CHAR_BIT >= INDEX_LIMIT_LOG2 + 1);

const JSCodeSpec js::CodeSpec[] = {
#define MAKE_CODESPEC(op, val, name, token, length, nuses, ndefs, format) \
  {length, nuses, ndefs, format},
    FOR_EACH_OPCODE(MAKE_CODESPEC)
#undef MAKE_CODESPEC
};

/*
 * Each element of the array is either a source literal associated with JS
 * bytecode or null.
 */
static const char* const CodeToken[] = {
#define TOKEN(op, val, name, token, ...) token,
    FOR_EACH_OPCODE(TOKEN)
#undef TOKEN
};

/*
 * Array of JS bytecode names used by PC count JSON, DEBUG-only Disassemble
 * and JIT debug spew.
 */
const char* const js::CodeName[] = {
#define OPNAME(op, val, name, ...) name,
    FOR_EACH_OPCODE(OPNAME)
#undef OPNAME
};

/************************************************************************/

static bool DecompileArgumentFromStack(JSContext* cx, int formalIndex,
                                       UniqueChars* res);

/* static */ const char PCCounts::numExecName[] = "interp";

static MOZ_MUST_USE bool DumpIonScriptCounts(Sprinter* sp, HandleScript script,
                                             jit::IonScriptCounts* ionCounts) {
  if (!sp->jsprintf("IonScript [%zu blocks]:\n", ionCounts->numBlocks())) {
    return false;
  }

  for (size_t i = 0; i < ionCounts->numBlocks(); i++) {
    const jit::IonBlockCounts& block = ionCounts->block(i);
    unsigned lineNumber = 0, columnNumber = 0;
    lineNumber = PCToLineNumber(script, script->offsetToPC(block.offset()),
                                &columnNumber);
    if (!sp->jsprintf("BB #%" PRIu32 " [%05u,%u,%u]", block.id(),
                      block.offset(), lineNumber, columnNumber)) {
      return false;
    }
    if (block.description()) {
      if (!sp->jsprintf(" [inlined %s]", block.description())) {
        return false;
      }
    }
    for (size_t j = 0; j < block.numSuccessors(); j++) {
      if (!sp->jsprintf(" -> #%" PRIu32, block.successor(j))) {
        return false;
      }
    }
    if (!sp->jsprintf(" :: %" PRIu64 " hits\n", block.hitCount())) {
      return false;
    }
    if (!sp->jsprintf("%s\n", block.code())) {
      return false;
    }
  }

  return true;
}

static MOZ_MUST_USE bool DumpPCCounts(JSContext* cx, HandleScript script,
                                      Sprinter* sp) {
  MOZ_ASSERT(script->hasScriptCounts());

#ifdef DEBUG
  jsbytecode* pc = script->code();
  while (pc < script->codeEnd()) {
    jsbytecode* next = GetNextPc(pc);

    if (!Disassemble1(cx, script, pc, script->pcToOffset(pc), true, sp)) {
      return false;
    }

    if (!sp->put("                  {")) {
      return false;
    }

    PCCounts* counts = script->maybeGetPCCounts(pc);
    if (double val = counts ? counts->numExec() : 0.0) {
      if (!sp->jsprintf("\"%s\": %.0f", PCCounts::numExecName, val)) {
        return false;
      }
    }
    if (!sp->put("}\n")) {
      return false;
    }

    pc = next;
  }
#endif

  jit::IonScriptCounts* ionCounts = script->getIonCounts();
  while (ionCounts) {
    if (!DumpIonScriptCounts(sp, script, ionCounts)) {
      return false;
    }

    ionCounts = ionCounts->previous();
  }

  return true;
}

bool js::DumpRealmPCCounts(JSContext* cx) {
  Rooted<GCVector<JSScript*>> scripts(cx, GCVector<JSScript*>(cx));
  for (auto iter = cx->zone()->cellIter<JSScript>(); !iter.done();
       iter.next()) {
    JSScript* script = iter;
    if (gc::IsAboutToBeFinalizedUnbarriered(&script)) {
      continue;
    }
    if (script->realm() != cx->realm()) {
      continue;
    }
    if (script->hasScriptCounts()) {
      if (!scripts.append(script)) {
        return false;
      }
    }
  }

  for (uint32_t i = 0; i < scripts.length(); i++) {
    HandleScript script = scripts[i];
    Sprinter sprinter(cx);
    if (!sprinter.init()) {
      return false;
    }

    fprintf(stdout, "--- SCRIPT %s:%u ---\n", script->filename(),
            script->lineno());
    if (!DumpPCCounts(cx, script, &sprinter)) {
      return false;
    }
    fputs(sprinter.string(), stdout);
    fprintf(stdout, "--- END SCRIPT %s:%u ---\n", script->filename(),
            script->lineno());
  }

  return true;
}

/////////////////////////////////////////////////////////////////////
// Bytecode Parser
/////////////////////////////////////////////////////////////////////

// Stores the information about the stack slot, where the value comes from.
// Elements of BytecodeParser::Bytecode.{offsetStack,offsetStackAfter} arrays.
struct OffsetAndDefIndex {
  // To make this struct a POD type, keep these properties public.
  // Use accessors instead of directly accessing them.

  // The offset of the PC that pushed the value for this slot.
  uint32_t offset_;

  // The index in `ndefs` for the PC (0-origin)
  uint8_t defIndex_;

  enum : uint8_t {
    Normal = 0,

    // Ignored this value in the expression decompilation.
    // Used by JSOP_NOP_DESTRUCTURING.  See BytecodeParser::simulateOp.
    Ignored,

    // The value in this slot comes from 2 or more paths.
    // offset_ and defIndex_ holds the information for the path that
    // reaches here first.
    Merged,
  } type_;

  uint32_t offset() const {
    MOZ_ASSERT(!isSpecial());
    return offset_;
  };
  uint32_t specialOffset() const {
    MOZ_ASSERT(isSpecial());
    return offset_;
  };

  uint8_t defIndex() const {
    MOZ_ASSERT(!isSpecial());
    return defIndex_;
  }
  uint8_t specialDefIndex() const {
    MOZ_ASSERT(isSpecial());
    return defIndex_;
  }

  bool isSpecial() const { return type_ != Normal; }
  bool isMerged() const { return type_ == Merged; }
  bool isIgnored() const { return type_ == Ignored; }

  void set(uint32_t aOffset, uint8_t aDefIndex) {
    offset_ = aOffset;
    defIndex_ = aDefIndex;
    type_ = Normal;
  }

  // Keep offset_ and defIndex_ values for stack dump.
  void setMerged() { type_ = Merged; }
  void setIgnored() { type_ = Ignored; }

  bool operator==(const OffsetAndDefIndex& rhs) const {
    return offset_ == rhs.offset_ && defIndex_ == rhs.defIndex_;
  }

  bool operator!=(const OffsetAndDefIndex& rhs) const {
    return !(*this == rhs);
  }
};

namespace mozilla {

template <>
struct IsPod<OffsetAndDefIndex> : TrueType {};

}  // namespace mozilla

namespace {

class BytecodeParser {
 public:
  enum class JumpKind {
    Simple,
    SwitchCase,
    SwitchDefault,
    TryCatch,
    TryFinally
  };

 private:
  class Bytecode {
   public:
    explicit Bytecode(const LifoAllocPolicy<Fallible>& alloc)
        : parsed(false),
          stackDepth(0),
          offsetStack(nullptr)
#if defined(DEBUG) || defined(JS_JITSPEW)
          ,
          stackDepthAfter(0),
          offsetStackAfter(nullptr),
          jumpOrigins(alloc)
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */
    {
    }

    // Whether this instruction has been analyzed to get its output defines
    // and stack.
    bool parsed;

    // Stack depth before this opcode.
    uint32_t stackDepth;

    // Pointer to array of |stackDepth| offsets.  An element at position N
    // in the array is the offset of the opcode that defined the
    // corresponding stack slot.  The top of the stack is at position
    // |stackDepth - 1|.
    OffsetAndDefIndex* offsetStack;

#if defined(DEBUG) || defined(JS_JITSPEW)
    // stack depth after this opcode.
    uint32_t stackDepthAfter;

    // Pointer to array of |stackDepthAfter| offsets.
    OffsetAndDefIndex* offsetStackAfter;

    struct JumpInfo {
      uint32_t from;
      JumpKind kind;

      JumpInfo(uint32_t from_, JumpKind kind_) : from(from_), kind(kind_) {}
    };

    // A list of offsets of the bytecode that jumps to this bytecode,
    // exclusing previous bytecode.
    Vector<JumpInfo, 0, LifoAllocPolicy<Fallible>> jumpOrigins;
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

    bool captureOffsetStack(LifoAlloc& alloc, const OffsetAndDefIndex* stack,
                            uint32_t depth) {
      stackDepth = depth;
      if (stackDepth) {
        offsetStack = alloc.newArray<OffsetAndDefIndex>(stackDepth);
        if (!offsetStack) {
          return false;
        }
        for (uint32_t n = 0; n < stackDepth; n++) {
          offsetStack[n] = stack[n];
        }
      }
      return true;
    }

#if defined(DEBUG) || defined(JS_JITSPEW)
    bool captureOffsetStackAfter(LifoAlloc& alloc,
                                 const OffsetAndDefIndex* stack,
                                 uint32_t depth) {
      stackDepthAfter = depth;
      if (stackDepthAfter) {
        offsetStackAfter = alloc.newArray<OffsetAndDefIndex>(stackDepthAfter);
        if (!offsetStackAfter) {
          return false;
        }
        for (uint32_t n = 0; n < stackDepthAfter; n++) {
          offsetStackAfter[n] = stack[n];
        }
      }
      return true;
    }

    bool addJump(uint32_t from, JumpKind kind) {
      return jumpOrigins.append(JumpInfo(from, kind));
    }
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

    // When control-flow merges, intersect the stacks, marking slots that
    // are defined by different offsets and/or defIndices merged.
    // This is sufficient for forward control-flow.  It doesn't grok loops
    // -- for that you would have to iterate to a fixed point -- but there
    // shouldn't be operands on the stack at a loop back-edge anyway.
    void mergeOffsetStack(const OffsetAndDefIndex* stack, uint32_t depth) {
      MOZ_ASSERT(depth == stackDepth);
      for (uint32_t n = 0; n < stackDepth; n++) {
        if (stack[n].isIgnored()) {
          continue;
        }
        if (offsetStack[n].isIgnored()) {
          offsetStack[n] = stack[n];
        }
        if (offsetStack[n] != stack[n]) {
          offsetStack[n].setMerged();
        }
      }
    }
  };

  JSContext* cx_;
  LifoAlloc& alloc_;
  RootedScript script_;

  Bytecode** codeArray_;

#if defined(DEBUG) || defined(JS_JITSPEW)
  // Dedicated mode for stack dump.
  // Capture stack after each opcode, and also enable special handling for
  // some opcodes to make stack transition clearer.
  bool isStackDump;
#endif

 public:
  BytecodeParser(JSContext* cx, LifoAlloc& alloc, JSScript* script)
      : cx_(cx),
        alloc_(alloc),
        script_(cx, script),
        codeArray_(nullptr)
#ifdef DEBUG
        ,
        isStackDump(false)
#endif
  {
  }

  bool parse();

#if defined(DEBUG) || defined(JS_JITSPEW)
  bool isReachable(const jsbytecode* pc) const { return maybeCode(pc); }
#endif

  uint32_t stackDepthAtPC(uint32_t offset) const {
    // Sometimes the code generator in debug mode asks about the stack depth
    // of unreachable code (bug 932180 comment 22).  Assume that unreachable
    // code has no operands on the stack.
    return getCode(offset).stackDepth;
  }
  uint32_t stackDepthAtPC(const jsbytecode* pc) const {
    return stackDepthAtPC(script_->pcToOffset(pc));
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  uint32_t stackDepthAfterPC(uint32_t offset) const {
    return getCode(offset).stackDepthAfter;
  }
  uint32_t stackDepthAfterPC(const jsbytecode* pc) const {
    return stackDepthAfterPC(script_->pcToOffset(pc));
  }
#endif

  const OffsetAndDefIndex& offsetForStackOperand(uint32_t offset,
                                                 int operand) const {
    Bytecode& code = getCode(offset);
    if (operand < 0) {
      operand += code.stackDepth;
      MOZ_ASSERT(operand >= 0);
    }
    MOZ_ASSERT(uint32_t(operand) < code.stackDepth);
    return code.offsetStack[operand];
  }
  jsbytecode* pcForStackOperand(jsbytecode* pc, int operand,
                                uint8_t* defIndex) const {
    size_t offset = script_->pcToOffset(pc);
    const OffsetAndDefIndex& offsetAndDefIndex =
        offsetForStackOperand(offset, operand);
    if (offsetAndDefIndex.isSpecial()) {
      return nullptr;
    }
    *defIndex = offsetAndDefIndex.defIndex();
    return script_->offsetToPC(offsetAndDefIndex.offset());
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  const OffsetAndDefIndex& offsetForStackOperandAfterPC(uint32_t offset,
                                                        int operand) const {
    Bytecode& code = getCode(offset);
    if (operand < 0) {
      operand += code.stackDepthAfter;
      MOZ_ASSERT(operand >= 0);
    }
    MOZ_ASSERT(uint32_t(operand) < code.stackDepthAfter);
    return code.offsetStackAfter[operand];
  }

  template <typename Callback>
  bool forEachJumpOrigins(jsbytecode* pc, Callback callback) const {
    Bytecode& code = getCode(script_->pcToOffset(pc));

    for (Bytecode::JumpInfo& info : code.jumpOrigins) {
      if (!callback(script_->offsetToPC(info.from), info.kind)) {
        return false;
      }
    }

    return true;
  }

  void setStackDump() { isStackDump = true; }
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

 private:
  LifoAlloc& alloc() { return alloc_; }

  void reportOOM() { ReportOutOfMemory(cx_); }

  uint32_t maximumStackDepth() const {
    return script_->nslots() - script_->nfixed();
  }

  Bytecode& getCode(uint32_t offset) const {
    MOZ_ASSERT(offset < script_->length());
    MOZ_ASSERT(codeArray_[offset]);
    return *codeArray_[offset];
  }

  Bytecode* maybeCode(uint32_t offset) const {
    MOZ_ASSERT(offset < script_->length());
    return codeArray_[offset];
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  Bytecode* maybeCode(const jsbytecode* pc) const {
    return maybeCode(script_->pcToOffset(pc));
  }
#endif

  uint32_t simulateOp(JSOp op, uint32_t offset, OffsetAndDefIndex* offsetStack,
                      uint32_t stackDepth);

  inline bool recordBytecode(uint32_t offset,
                             const OffsetAndDefIndex* offsetStack,
                             uint32_t stackDepth);

  inline bool addJump(uint32_t offset, uint32_t* currentOffset,
                      uint32_t stackDepth, const OffsetAndDefIndex* offsetStack,
                      jsbytecode* pc, JumpKind kind);
};

}  // anonymous namespace

uint32_t BytecodeParser::simulateOp(JSOp op, uint32_t offset,
                                    OffsetAndDefIndex* offsetStack,
                                    uint32_t stackDepth) {
  jsbytecode* pc = script_->offsetToPC(offset);
  uint32_t nuses = GetUseCount(pc);
  uint32_t ndefs = GetDefCount(pc);

  MOZ_ASSERT(stackDepth >= nuses);
  stackDepth -= nuses;
  MOZ_ASSERT(stackDepth + ndefs <= maximumStackDepth());

#ifdef DEBUG
  if (isStackDump) {
    // Opcodes that modifies the object but keeps it on the stack while
    // initialization should be listed here instead of switch below.
    // For error message, they shouldn't be shown as the original object
    // after adding properties.
    // For stack dump, keeping the input is better.
    switch (op) {
      case JSOP_INITHIDDENPROP:
      case JSOP_INITHIDDENPROP_GETTER:
      case JSOP_INITHIDDENPROP_SETTER:
      case JSOP_INITLOCKEDPROP:
      case JSOP_INITPROP:
      case JSOP_INITPROP_GETTER:
      case JSOP_INITPROP_SETTER:
      case JSOP_SETFUNNAME:
        // Keep the second value.
        MOZ_ASSERT(nuses == 2);
        MOZ_ASSERT(ndefs == 1);
        goto end;

      case JSOP_INITELEM:
      case JSOP_INITELEM_GETTER:
      case JSOP_INITELEM_SETTER:
      case JSOP_INITHIDDENELEM:
      case JSOP_INITHIDDENELEM_GETTER:
      case JSOP_INITHIDDENELEM_SETTER:
        // Keep the third value.
        MOZ_ASSERT(nuses == 3);
        MOZ_ASSERT(ndefs == 1);
        goto end;

      default:
        break;
    }
  }
#endif /* DEBUG */

  // Mark the current offset as defining its values on the offset stack,
  // unless it just reshuffles the stack.  In that case we want to preserve
  // the opcode that generated the original value.
  switch (op) {
    default:
      for (uint32_t n = 0; n != ndefs; ++n) {
        offsetStack[stackDepth + n].set(offset, n);
      }
      break;

    case JSOP_NOP_DESTRUCTURING:
      // Poison the last offset to not obfuscate the error message.
      offsetStack[stackDepth - 1].setIgnored();
      break;

    case JSOP_CASE:
      // Keep the switch value.
      MOZ_ASSERT(ndefs == 1);
      break;

    case JSOP_DUP:
      MOZ_ASSERT(ndefs == 2);
      offsetStack[stackDepth + 1] = offsetStack[stackDepth];
      break;

    case JSOP_DUP2:
      MOZ_ASSERT(ndefs == 4);
      offsetStack[stackDepth + 2] = offsetStack[stackDepth];
      offsetStack[stackDepth + 3] = offsetStack[stackDepth + 1];
      break;

    case JSOP_DUPAT: {
      MOZ_ASSERT(ndefs == 1);
      unsigned n = GET_UINT24(pc);
      MOZ_ASSERT(n < stackDepth);
      offsetStack[stackDepth] = offsetStack[stackDepth - 1 - n];
      break;
    }

    case JSOP_SWAP: {
      MOZ_ASSERT(ndefs == 2);
      OffsetAndDefIndex tmp = offsetStack[stackDepth + 1];
      offsetStack[stackDepth + 1] = offsetStack[stackDepth];
      offsetStack[stackDepth] = tmp;
      break;
    }

    case JSOP_PICK: {
      unsigned n = GET_UINT8(pc);
      MOZ_ASSERT(ndefs == n + 1);
      uint32_t top = stackDepth + n;
      OffsetAndDefIndex tmp = offsetStack[stackDepth];
      for (uint32_t i = stackDepth; i < top; i++) {
        offsetStack[i] = offsetStack[i + 1];
      }
      offsetStack[top] = tmp;
      break;
    }

    case JSOP_UNPICK: {
      unsigned n = GET_UINT8(pc);
      MOZ_ASSERT(ndefs == n + 1);
      uint32_t top = stackDepth + n;
      OffsetAndDefIndex tmp = offsetStack[top];
      for (uint32_t i = top; i > stackDepth; i--) {
        offsetStack[i] = offsetStack[i - 1];
      }
      offsetStack[stackDepth] = tmp;
      break;
    }

    case JSOP_AND:
    case JSOP_CHECKISOBJ:
    case JSOP_CHECKISCALLABLE:
    case JSOP_CHECKOBJCOERCIBLE:
    case JSOP_CHECKTHIS:
    case JSOP_CHECKTHISREINIT:
    case JSOP_CHECKCLASSHERITAGE:
    case JSOP_DEBUGCHECKSELFHOSTED:
    case JSOP_INITGLEXICAL:
    case JSOP_INITLEXICAL:
    case JSOP_OR:
    case JSOP_SETALIASEDVAR:
    case JSOP_SETARG:
    case JSOP_SETINTRINSIC:
    case JSOP_SETLOCAL:
    case JSOP_THROWSETALIASEDCONST:
    case JSOP_THROWSETCALLEE:
    case JSOP_THROWSETCONST:
    case JSOP_INITALIASEDLEXICAL:
    case JSOP_INITIALYIELD:
    case JSOP_ITERNEXT:
      // Keep the top value.
      MOZ_ASSERT(nuses == 1);
      MOZ_ASSERT(ndefs == 1);
      break;

    case JSOP_INITHOMEOBJECT:
      // Pop the top value, keep the other value.
      MOZ_ASSERT(nuses == 2);
      MOZ_ASSERT(ndefs == 1);
      break;

    case JSOP_SETGNAME:
    case JSOP_SETNAME:
    case JSOP_SETPROP:
    case JSOP_STRICTSETGNAME:
    case JSOP_STRICTSETNAME:
    case JSOP_STRICTSETPROP:
      // Keep the top value, removing other 1 value.
      MOZ_ASSERT(nuses == 2);
      MOZ_ASSERT(ndefs == 1);
      offsetStack[stackDepth] = offsetStack[stackDepth + 1];
      break;

    case JSOP_SETPROP_SUPER:
    case JSOP_STRICTSETPROP_SUPER:
      // Keep the top value, removing other 2 values.
      MOZ_ASSERT(nuses == 3);
      MOZ_ASSERT(ndefs == 1);
      offsetStack[stackDepth] = offsetStack[stackDepth + 2];
      break;

    case JSOP_SETELEM_SUPER:
    case JSOP_STRICTSETELEM_SUPER:
      // Keep the top value, removing other 3 values.
      MOZ_ASSERT(nuses == 4);
      MOZ_ASSERT(ndefs == 1);
      offsetStack[stackDepth] = offsetStack[stackDepth + 3];
      break;

    case JSOP_ISGENCLOSING:
    case JSOP_ISNOITER:
    case JSOP_MOREITER:
    case JSOP_OPTIMIZE_SPREADCALL:
      // Keep the top value and push one more value.
      MOZ_ASSERT(nuses == 1);
      MOZ_ASSERT(ndefs == 2);
      offsetStack[stackDepth + 1].set(offset, 1);
      break;
  }

#ifdef DEBUG
end:
#endif /* DEBUG */

  stackDepth += ndefs;
  return stackDepth;
}

bool BytecodeParser::recordBytecode(uint32_t offset,
                                    const OffsetAndDefIndex* offsetStack,
                                    uint32_t stackDepth) {
  MOZ_ASSERT(offset < script_->length());

  Bytecode*& code = codeArray_[offset];
  if (!code) {
    code = alloc().new_<Bytecode>(alloc());
    if (!code || !code->captureOffsetStack(alloc(), offsetStack, stackDepth)) {
      reportOOM();
      return false;
    }
  } else {
    code->mergeOffsetStack(offsetStack, stackDepth);
  }

  return true;
}

bool BytecodeParser::addJump(uint32_t offset, uint32_t* currentOffset,
                             uint32_t stackDepth,
                             const OffsetAndDefIndex* offsetStack,
                             jsbytecode* pc, JumpKind kind) {
  if (!recordBytecode(offset, offsetStack, stackDepth)) {
    return false;
  }

#ifdef DEBUG
  if (isStackDump) {
    if (!codeArray_[offset]->addJump(script_->pcToOffset(pc), kind)) {
      reportOOM();
      return false;
    }
  }
#endif /* DEBUG */

  Bytecode*& code = codeArray_[offset];
  if (offset < *currentOffset && !code->parsed) {
    // Backedge in a while/for loop, whose body has not been parsed due
    // to a lack of fallthrough at the loop head. Roll back the offset
    // to analyze the body.
    *currentOffset = offset;
  }

  return true;
}

bool BytecodeParser::parse() {
  MOZ_ASSERT(!codeArray_);

  uint32_t length = script_->length();
  codeArray_ = alloc().newArray<Bytecode*>(length);

  if (!codeArray_) {
    reportOOM();
    return false;
  }

  mozilla::PodZero(codeArray_, length);

  // Fill in stack depth and definitions at initial bytecode.
  Bytecode* startcode = alloc().new_<Bytecode>(alloc());
  if (!startcode) {
    reportOOM();
    return false;
  }

  // Fill in stack depth and definitions at initial bytecode.
  OffsetAndDefIndex* offsetStack =
      alloc().newArray<OffsetAndDefIndex>(maximumStackDepth());
  if (maximumStackDepth() && !offsetStack) {
    reportOOM();
    return false;
  }

  startcode->stackDepth = 0;
  codeArray_[0] = startcode;

  uint32_t offset, nextOffset = 0;
  while (nextOffset < length) {
    offset = nextOffset;

    Bytecode* code = maybeCode(offset);
    jsbytecode* pc = script_->offsetToPC(offset);

    JSOp op = (JSOp)*pc;
    MOZ_ASSERT(op < JSOP_LIMIT);

    // Immediate successor of this bytecode.
    uint32_t successorOffset = offset + GetBytecodeLength(pc);

    // Next bytecode to analyze.  This is either the successor, or is an
    // earlier bytecode if this bytecode has a loop backedge.
    nextOffset = successorOffset;

    if (!code) {
      // Haven't found a path by which this bytecode is reachable.
      continue;
    }

    // On a jump target, we reload the offsetStack saved for the current
    // bytecode, as it contains either the original offset stack, or the
    // merged offset stack.
    if (BytecodeIsJumpTarget(op)) {
      for (uint32_t n = 0; n < code->stackDepth; ++n) {
        offsetStack[n] = code->offsetStack[n];
      }
    }

    if (code->parsed) {
      // No need to reparse.
      continue;
    }

    code->parsed = true;

    uint32_t stackDepth = simulateOp(op, offset, offsetStack, code->stackDepth);

#ifdef DEBUG
    if (isStackDump) {
      if (!code->captureOffsetStackAfter(alloc(), offsetStack, stackDepth)) {
        reportOOM();
        return false;
      }
    }
#endif /* DEBUG */

    switch (op) {
      case JSOP_TABLESWITCH: {
        uint32_t defaultOffset = offset + GET_JUMP_OFFSET(pc);
        jsbytecode* pc2 = pc + JUMP_OFFSET_LEN;
        int32_t low = GET_JUMP_OFFSET(pc2);
        pc2 += JUMP_OFFSET_LEN;
        int32_t high = GET_JUMP_OFFSET(pc2);
        pc2 += JUMP_OFFSET_LEN;

        if (!addJump(defaultOffset, &nextOffset, stackDepth, offsetStack, pc,
                     JumpKind::SwitchDefault)) {
          return false;
        }

        uint32_t ncases = high - low + 1;

        for (uint32_t i = 0; i < ncases; i++) {
          uint32_t targetOffset = script_->tableSwitchCaseOffset(pc, i);
          if (targetOffset != defaultOffset) {
            if (!addJump(targetOffset, &nextOffset, stackDepth, offsetStack, pc,
                         JumpKind::SwitchCase)) {
              return false;
            }
          }
        }
        break;
      }

      case JSOP_TRY: {
        // Everything between a try and corresponding catch or finally is
        // conditional. Note that there is no problem with code which is skipped
        // by a thrown exception but is not caught by a later handler in the
        // same function: no more code will execute, and it does not matter what
        // is defined.
        for (const JSTryNote& tn : script_->trynotes()) {
          if (tn.start == offset + 1) {
            uint32_t catchOffset = tn.start + tn.length;
            if (tn.kind == JSTRY_CATCH) {
              if (!addJump(catchOffset, &nextOffset, stackDepth, offsetStack,
                           pc, JumpKind::TryCatch)) {
                return false;
              }
            } else if (tn.kind == JSTRY_FINALLY) {
              if (!addJump(catchOffset, &nextOffset, stackDepth, offsetStack,
                           pc, JumpKind::TryFinally)) {
                return false;
              }
            }
          }
        }
        break;
      }

      default:
        break;
    }

    // Check basic jump opcodes, which may or may not have a fallthrough.
    if (IsJumpOpcode(op)) {
      // Case instructions do not push the lvalue back when branching.
      uint32_t newStackDepth = stackDepth;
      if (op == JSOP_CASE) {
        newStackDepth--;
      }

      uint32_t targetOffset = offset + GET_JUMP_OFFSET(pc);
      if (!addJump(targetOffset, &nextOffset, newStackDepth, offsetStack, pc,
                   JumpKind::Simple)) {
        return false;
      }
    }

    // Handle any fallthrough from this opcode.
    if (BytecodeFallsThrough(op)) {
      if (!recordBytecode(successorOffset, offsetStack, stackDepth)) {
        return false;
      }
    }
  }

  return true;
}

#if defined(DEBUG) || defined(JS_JITSPEW)

bool js::ReconstructStackDepth(JSContext* cx, JSScript* script, jsbytecode* pc,
                               uint32_t* depth, bool* reachablePC) {
  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  BytecodeParser parser(cx, allocScope.alloc(), script);
  if (!parser.parse()) {
    return false;
  }

  *reachablePC = parser.isReachable(pc);

  if (*reachablePC) {
    *depth = parser.stackDepthAtPC(pc);
  }

  return true;
}

static unsigned Disassemble1(JSContext* cx, HandleScript script, jsbytecode* pc,
                             unsigned loc, bool lines,
                             const BytecodeParser* parser, Sprinter* sp);

/*
 * If pc != nullptr, include a prefix indicating whether the PC is at the
 * current line. If showAll is true, include the source note type and the
 * entry stack depth.
 */
static MOZ_MUST_USE bool DisassembleAtPC(JSContext* cx, JSScript* scriptArg,
                                         bool lines, jsbytecode* pc,
                                         bool showAll, Sprinter* sp) {
  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  RootedScript script(cx, scriptArg);
  BytecodeParser parser(cx, allocScope.alloc(), script);
  parser.setStackDump();
  if (!parser.parse()) {
    return false;
  }

  if (showAll) {
    if (!sp->jsprintf("%s:%u\n", script->filename(),
                      unsigned(script->lineno()))) {
      return false;
    }
  }

  if (pc != nullptr) {
    if (!sp->put("    ")) {
      return false;
    }
  }
  if (showAll) {
    if (!sp->put("sn stack ")) {
      return false;
    }
  }
  if (!sp->put("loc   ")) {
    return false;
  }
  if (lines) {
    if (!sp->put("line")) {
      return false;
    }
  }
  if (!sp->put("  op\n")) {
    return false;
  }

  if (pc != nullptr) {
    if (!sp->put("    ")) {
      return false;
    }
  }
  if (showAll) {
    if (!sp->put("-- ----- ")) {
      return false;
    }
  }
  if (!sp->put("----- ")) {
    return false;
  }
  if (lines) {
    if (!sp->put("----")) {
      return false;
    }
  }
  if (!sp->put("  --\n")) {
    return false;
  }

  jsbytecode* next = script->code();
  jsbytecode* end = script->codeEnd();
  while (next < end) {
    if (next == script->main()) {
      if (!sp->put("main:\n")) {
        return false;
      }
    }
    if (pc != nullptr) {
      if (!sp->put(pc == next ? "--> " : "    ")) {
        return false;
      }
    }
    if (showAll) {
      jssrcnote* sn = GetSrcNote(cx, script, next);
      if (sn) {
        MOZ_ASSERT(!SN_IS_TERMINATOR(sn));
        jssrcnote* next = SN_NEXT(sn);
        while (!SN_IS_TERMINATOR(next) && SN_DELTA(next) == 0) {
          if (!sp->jsprintf("%02u\n    ", SN_TYPE(sn))) {
            return false;
          }
          sn = next;
          next = SN_NEXT(sn);
        }
        if (!sp->jsprintf("%02u ", SN_TYPE(sn))) {
          return false;
        }
      } else {
        if (!sp->put("   ")) {
          return false;
        }
      }
      if (parser.isReachable(next)) {
        if (!sp->jsprintf("%05u ", parser.stackDepthAtPC(next))) {
          return false;
        }
      } else {
        if (!sp->put("      ")) {
          return false;
        }
      }
    }
    unsigned len = Disassemble1(cx, script, next, script->pcToOffset(next),
                                lines, &parser, sp);
    if (!len) {
      return false;
    }

    next += len;
  }

  return true;
}

bool js::Disassemble(JSContext* cx, HandleScript script, bool lines,
                     Sprinter* sp) {
  return DisassembleAtPC(cx, script, lines, nullptr, false, sp);
}

JS_FRIEND_API bool js::DumpPC(JSContext* cx, FILE* fp) {
  gc::AutoSuppressGC suppressGC(cx);
  Sprinter sprinter(cx);
  if (!sprinter.init()) {
    return false;
  }
  ScriptFrameIter iter(cx);
  if (iter.done()) {
    fprintf(fp, "Empty stack.\n");
    return true;
  }
  RootedScript script(cx, iter.script());
  bool ok = DisassembleAtPC(cx, script, true, iter.pc(), false, &sprinter);
  fprintf(fp, "%s", sprinter.string());
  return ok;
}

JS_FRIEND_API bool js::DumpScript(JSContext* cx, JSScript* scriptArg,
                                  FILE* fp) {
  gc::AutoSuppressGC suppressGC(cx);
  Sprinter sprinter(cx);
  if (!sprinter.init()) {
    return false;
  }
  RootedScript script(cx, scriptArg);
  bool ok = Disassemble(cx, script, true, &sprinter);
  fprintf(fp, "%s", sprinter.string());
  return ok;
}

static UniqueChars ToDisassemblySource(JSContext* cx, HandleValue v) {
  if (v.isString()) {
    return QuoteString(cx, v.toString(), '"');
  }

  if (JS::RuntimeHeapIsBusy()) {
    return DuplicateString(cx, "<value>");
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();

    if (obj.is<JSFunction>()) {
      RootedFunction fun(cx, &obj.as<JSFunction>());
      JSString* str = JS_DecompileFunction(cx, fun);
      if (!str) {
        return nullptr;
      }
      return StringToNewUTF8CharsZ(cx, *str);
    }

    if (obj.is<RegExpObject>()) {
      JSString* source = obj.as<RegExpObject>().toString(cx);
      if (!source) {
        return nullptr;
      }
      return StringToNewUTF8CharsZ(cx, *source);
    }
  }

  JSString* str = ValueToSource(cx, v);
  if (!str) {
    return nullptr;
  }
  return QuoteString(cx, str);
}

static bool ToDisassemblySource(JSContext* cx, HandleScope scope,
                                UniqueChars* bytes) {
  UniqueChars source = JS_smprintf("%s {", ScopeKindString(scope->kind()));
  if (!source) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
    UniqueChars nameBytes = AtomToPrintableString(cx, bi.name());
    if (!nameBytes) {
      return false;
    }

    source = JS_sprintf_append(std::move(source), "%s: ", nameBytes.get());
    if (!source) {
      ReportOutOfMemory(cx);
      return false;
    }

    BindingLocation loc = bi.location();
    switch (loc.kind()) {
      case BindingLocation::Kind::Global:
        source = JS_sprintf_append(std::move(source), "global");
        break;

      case BindingLocation::Kind::Frame:
        source =
            JS_sprintf_append(std::move(source), "frame slot %u", loc.slot());
        break;

      case BindingLocation::Kind::Environment:
        source =
            JS_sprintf_append(std::move(source), "env slot %u", loc.slot());
        break;

      case BindingLocation::Kind::Argument:
        source =
            JS_sprintf_append(std::move(source), "arg slot %u", loc.slot());
        break;

      case BindingLocation::Kind::NamedLambdaCallee:
        source = JS_sprintf_append(std::move(source), "named lambda callee");
        break;

      case BindingLocation::Kind::Import:
        source = JS_sprintf_append(std::move(source), "import");
        break;
    }

    if (!source) {
      ReportOutOfMemory(cx);
      return false;
    }

    if (!bi.isLast()) {
      source = JS_sprintf_append(std::move(source), ", ");
      if (!source) {
        ReportOutOfMemory(cx);
        return false;
      }
    }
  }

  source = JS_sprintf_append(std::move(source), "}");
  if (!source) {
    ReportOutOfMemory(cx);
    return false;
  }

  *bytes = std::move(source);
  return true;
}

static bool DumpJumpOrigins(HandleScript script, jsbytecode* pc,
                            const BytecodeParser* parser, Sprinter* sp) {
  bool called = false;
  auto callback = [&script, &sp, &called](jsbytecode* pc,
                                          BytecodeParser::JumpKind kind) {
    if (!called) {
      called = true;
      if (!sp->put("\n# ")) {
        return false;
      }
    } else {
      if (!sp->put(", ")) {
        return false;
      }
    }

    switch (kind) {
      case BytecodeParser::JumpKind::Simple:
        break;

      case BytecodeParser::JumpKind::SwitchCase:
        if (!sp->put("switch-case ")) {
          return false;
        }
        break;

      case BytecodeParser::JumpKind::SwitchDefault:
        if (!sp->put("switch-default ")) {
          return false;
        }
        break;

      case BytecodeParser::JumpKind::TryCatch:
        if (!sp->put("try-catch ")) {
          return false;
        }
        break;

      case BytecodeParser::JumpKind::TryFinally:
        if (!sp->put("try-finally ")) {
          return false;
        }
        break;
    }

    if (!sp->jsprintf("from %s @ %05u", CodeName[*pc],
                      unsigned(script->pcToOffset(pc)))) {
      return false;
    }

    return true;
  };
  if (!parser->forEachJumpOrigins(pc, callback)) {
    return false;
  }
  if (called) {
    if (!sp->put("\n")) {
      return false;
    }
  }

  return true;
}

static bool DecompileAtPCForStackDump(
    JSContext* cx, HandleScript script,
    const OffsetAndDefIndex& offsetAndDefIndex, Sprinter* sp);

static unsigned Disassemble1(JSContext* cx, HandleScript script, jsbytecode* pc,
                             unsigned loc, bool lines,
                             const BytecodeParser* parser, Sprinter* sp) {
  if (parser && parser->isReachable(pc)) {
    if (!DumpJumpOrigins(script, pc, parser, sp)) {
      return 0;
    }
  }

  size_t before = sp->stringEnd() - sp->string();
  bool stackDumped = false;
  auto dumpStack = [&cx, &script, &pc, &parser, &sp, &before, &stackDumped]() {
    if (!parser) {
      return true;
    }
    if (stackDumped) {
      return true;
    }
    stackDumped = true;

    size_t after = sp->stringEnd() - sp->string();
    MOZ_ASSERT(after >= before);

    static const size_t stack_column = 40;
    for (size_t i = after - before; i < stack_column - 1; i++) {
      if (!sp->put(" ")) {
        return false;
      }
    }

    if (!sp->put(" # ")) {
      return false;
    }

    if (!parser->isReachable(pc)) {
      if (!sp->put("!!! UNREACHABLE !!!")) {
        return false;
      }
    } else {
      uint32_t depth = parser->stackDepthAfterPC(pc);

      for (uint32_t i = 0; i < depth; i++) {
        if (i) {
          if (!sp->put(" ")) {
            return false;
          }
        }

        const OffsetAndDefIndex& offsetAndDefIndex =
            parser->offsetForStackOperandAfterPC(script->pcToOffset(pc), i);
        // This will decompile the stack for the same PC many times.
        // We'll avoid optimizing it since this is a testing function
        // and it won't be worth managing cached expression here.
        if (!DecompileAtPCForStackDump(cx, script, offsetAndDefIndex, sp)) {
          return false;
        }
      }
    }

    return true;
  };

  JSOp op = (JSOp)*pc;
  if (op >= JSOP_LIMIT) {
    char numBuf1[12], numBuf2[12];
    SprintfLiteral(numBuf1, "%d", op);
    SprintfLiteral(numBuf2, "%d", JSOP_LIMIT);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BYTECODE_TOO_BIG, numBuf1, numBuf2);
    return 0;
  }
  const JSCodeSpec* cs = &CodeSpec[op];
  const unsigned len = cs->length;
  if (!sp->jsprintf("%05u:", loc)) {
    return 0;
  }
  if (lines) {
    if (!sp->jsprintf("%4u", PCToLineNumber(script, pc))) {
      return 0;
    }
  }
  if (!sp->jsprintf("  %s", CodeName[op])) {
    return 0;
  }

  int i;
  switch (JOF_TYPE(cs->format)) {
    case JOF_BYTE:
      // Scan the trynotes to find the associated catch block
      // and make the try opcode look like a jump instruction
      // with an offset. This simplifies code coverage analysis
      // based on this disassembled output.
      if (op == JSOP_TRY) {
        for (const JSTryNote& tn : script->trynotes()) {
          if (tn.kind == JSTRY_CATCH && tn.start == loc + 1) {
            if (!sp->jsprintf(" %u (%+d)", unsigned(loc + tn.length + 1),
                              int(tn.length + 1))) {
              return 0;
            }
            break;
          }
        }
      }
      break;

    case JOF_JUMP: {
      ptrdiff_t off = GET_JUMP_OFFSET(pc);
      if (!sp->jsprintf(" %u (%+d)", unsigned(loc + int(off)), int(off))) {
        return 0;
      }
      break;
    }

    case JOF_SCOPE: {
      RootedScope scope(cx, script->getScope(GET_UINT32_INDEX(pc)));
      UniqueChars bytes;
      if (!ToDisassemblySource(cx, scope, &bytes)) {
        return 0;
      }
      if (!sp->jsprintf(" %s", bytes.get())) {
        return 0;
      }
      break;
    }

    case JOF_ENVCOORD: {
      RootedValue v(cx, StringValue(EnvironmentCoordinateNameSlow(script, pc)));
      UniqueChars bytes = ToDisassemblySource(cx, v);
      if (!bytes) {
        return 0;
      }
      EnvironmentCoordinate ec(pc);
      if (!sp->jsprintf(" %s (hops = %u, slot = %u)", bytes.get(), ec.hops(),
                        ec.slot())) {
        return 0;
      }
      break;
    }

    case JOF_ATOM: {
      RootedValue v(cx, StringValue(script->getAtom(GET_UINT32_INDEX(pc))));
      UniqueChars bytes = ToDisassemblySource(cx, v);
      if (!bytes) {
        return 0;
      }
      if (!sp->jsprintf(" %s", bytes.get())) {
        return 0;
      }
      break;
    }

#  ifdef ENABLE_BIGINT
    case JOF_BIGINT:
      // Fallthrough.
#  endif
    case JOF_DOUBLE: {
      RootedValue v(cx, script->getConst(GET_UINT32_INDEX(pc)));
      UniqueChars bytes = ToDisassemblySource(cx, v);
      if (!bytes) {
        return 0;
      }
      if (!sp->jsprintf(" %s", bytes.get())) {
        return 0;
      }
      break;
    }

    case JOF_OBJECT: {
      /* Don't call obj.toSource if analysis/inference is active. */
      if (script->zone()->types.activeAnalysis) {
        if (!sp->jsprintf(" object")) {
          return 0;
        }
        break;
      }

      JSObject* obj = script->getObject(GET_UINT32_INDEX(pc));
      {
        RootedValue v(cx, ObjectValue(*obj));
        UniqueChars bytes = ToDisassemblySource(cx, v);
        if (!bytes) {
          return 0;
        }
        if (!sp->jsprintf(" %s", bytes.get())) {
          return 0;
        }
      }
      break;
    }

    case JOF_REGEXP: {
      js::RegExpObject* obj = script->getRegExp(pc);
      RootedValue v(cx, ObjectValue(*obj));
      UniqueChars bytes = ToDisassemblySource(cx, v);
      if (!bytes) {
        return 0;
      }
      if (!sp->jsprintf(" %s", bytes.get())) {
        return 0;
      }
      break;
    }

    case JOF_TABLESWITCH: {
      int32_t i, low, high;

      ptrdiff_t off = GET_JUMP_OFFSET(pc);
      jsbytecode* pc2 = pc + JUMP_OFFSET_LEN;
      low = GET_JUMP_OFFSET(pc2);
      pc2 += JUMP_OFFSET_LEN;
      high = GET_JUMP_OFFSET(pc2);
      pc2 += JUMP_OFFSET_LEN;
      if (!sp->jsprintf(" defaultOffset %d low %d high %d", int(off), low,
                        high)) {
        return 0;
      }

      // Display stack dump before diplaying the offsets for each case.
      if (!dumpStack()) {
        return 0;
      }

      for (i = low; i <= high; i++) {
        off =
            script->tableSwitchCaseOffset(pc, i - low) - script->pcToOffset(pc);
        if (!sp->jsprintf("\n\t%d: %d", i, int(off))) {
          return 0;
        }
      }
      break;
    }

    case JOF_QARG:
      if (!sp->jsprintf(" %u", GET_ARGNO(pc))) {
        return 0;
      }
      break;

    case JOF_LOCAL:
      if (!sp->jsprintf(" %u", GET_LOCALNO(pc))) {
        return 0;
      }
      break;

    case JOF_UINT32:
      if (!sp->jsprintf(" %u", GET_UINT32(pc))) {
        return 0;
      }
      break;

    case JOF_ARGC:
    case JOF_UINT16:
      i = (int)GET_UINT16(pc);
      goto print_int;

    case JOF_RESUMEINDEX:
    case JOF_UINT24:
      MOZ_ASSERT(len == 4);
      i = (int)GET_UINT24(pc);
      goto print_int;

    case JOF_UINT8:
      i = GET_UINT8(pc);
      goto print_int;

    case JOF_INT8:
      i = GET_INT8(pc);
      goto print_int;

    case JOF_INT32:
      MOZ_ASSERT(op == JSOP_INT32);
      i = GET_INT32(pc);
    print_int:
      if (!sp->jsprintf(" %d", i)) {
        return 0;
      }
      break;

    default: {
      char numBuf[12];
      SprintfLiteral(numBuf, "%x", cs->format);
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_UNKNOWN_FORMAT, numBuf);
      return 0;
    }
  }

  if (!dumpStack()) {
    return 0;
  }

  if (!sp->put("\n")) {
    return 0;
  }
  return len;
}

unsigned js::Disassemble1(JSContext* cx, JS::Handle<JSScript*> script,
                          jsbytecode* pc, unsigned loc, bool lines,
                          Sprinter* sp) {
  return Disassemble1(cx, script, pc, loc, lines, nullptr, sp);
}

#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

namespace {
/*
 * The expression decompiler is invoked by error handling code to produce a
 * string representation of the erroring expression. As it's only a debugging
 * tool, it only supports basic expressions. For anything complicated, it simply
 * puts "(intermediate value)" into the error result.
 *
 * Here's the basic algorithm:
 *
 * 1. Find the stack location of the value whose expression we wish to
 * decompile. The error handler can explicitly pass this as an
 * argument. Otherwise, we search backwards down the stack for the offending
 * value.
 *
 * 2. Instantiate and run a BytecodeParser for the current frame. This creates a
 * stack of pcs parallel to the interpreter stack; given an interpreter stack
 * location, the corresponding pc stack location contains the opcode that pushed
 * the value in the interpreter. Now, with the result of step 1, we have the
 * opcode responsible for pushing the value we want to decompile.
 *
 * 3. Pass the opcode to decompilePC. decompilePC is the main decompiler
 * routine, responsible for a string representation of the expression that
 * generated a certain stack location. decompilePC looks at one opcode and
 * returns the JS source equivalent of that opcode.
 *
 * 4. Expressions can, of course, contain subexpressions. For example, the
 * literals "4" and "5" are subexpressions of the addition operator in "4 +
 * 5". If we need to decompile a subexpression, we call decompilePC (step 2)
 * recursively on the operands' pcs. The result is a depth-first traversal of
 * the expression tree.
 *
 */
struct ExpressionDecompiler {
  JSContext* cx;
  RootedScript script;
  const BytecodeParser& parser;
  Sprinter sprinter;

#if defined(DEBUG) || defined(JS_JITSPEW)
  // Dedicated mode for stack dump.
  // Generates an expression for stack dump, including internal state,
  // and also disables special handling for self-hosted code.
  bool isStackDump;
#endif

  ExpressionDecompiler(JSContext* cx, JSScript* script,
                       const BytecodeParser& parser)
      : cx(cx),
        script(cx, script),
        parser(parser),
        sprinter(cx)
#if defined(DEBUG) || defined(JS_JITSPEW)
        ,
        isStackDump(false)
#endif
  {
  }
  bool init();
  bool decompilePCForStackOperand(jsbytecode* pc, int i);
  bool decompilePC(jsbytecode* pc, uint8_t defIndex);
  bool decompilePC(const OffsetAndDefIndex& offsetAndDefIndex);
  JSAtom* getArg(unsigned slot);
  JSAtom* loadAtom(jsbytecode* pc);
  bool quote(JSString* s, char quote);
  bool write(const char* s);
  bool write(JSString* str);
  UniqueChars getOutput();
#if defined(DEBUG) || defined(JS_JITSPEW)
  void setStackDump() { isStackDump = true; }
#endif
};

bool ExpressionDecompiler::decompilePCForStackOperand(jsbytecode* pc, int i) {
  return decompilePC(parser.offsetForStackOperand(script->pcToOffset(pc), i));
}

bool ExpressionDecompiler::decompilePC(jsbytecode* pc, uint8_t defIndex) {
  MOZ_ASSERT(script->containsPC(pc));

  JSOp op = (JSOp)*pc;

  if (const char* token = CodeToken[op]) {
    MOZ_ASSERT(defIndex == 0);
    MOZ_ASSERT(CodeSpec[op].ndefs == 1);

    // Handle simple cases of binary and unary operators.
    switch (CodeSpec[op].nuses) {
      case 2: {
        jssrcnote* sn = GetSrcNote(cx, script, pc);
        if (!sn || SN_TYPE(sn) != SRC_ASSIGNOP) {
          return write("(") && decompilePCForStackOperand(pc, -2) &&
                 write(" ") && write(token) && write(" ") &&
                 decompilePCForStackOperand(pc, -1) && write(")");
        }
        break;
      }
      case 1:
        return write("(") && write(token) &&
               decompilePCForStackOperand(pc, -1) && write(")");
      default:
        break;
    }
  }

  switch (op) {
    case JSOP_DELNAME:
      return write("(delete ") && write(loadAtom(pc)) && write(")");

    case JSOP_GETGNAME:
    case JSOP_GETNAME:
    case JSOP_GETINTRINSIC:
      return write(loadAtom(pc));
    case JSOP_GETARG: {
      unsigned slot = GET_ARGNO(pc);

      // For self-hosted scripts that are called from non-self-hosted code,
      // decompiling the parameter name in the self-hosted script is
      // unhelpful. Decompile the argument name instead.
      if (script->selfHosted()
#ifdef DEBUG
          // For stack dump, argument name is not necessary.
          && !isStackDump
#endif /* DEBUG */
      ) {
        UniqueChars result;
        if (!DecompileArgumentFromStack(cx, slot, &result)) {
          return false;
        }

        // Note that decompiling the argument in the parent frame might
        // not succeed.
        if (result) {
          return write(result.get());
        }
      }

      JSAtom* atom = getArg(slot);
      if (!atom) {
        return false;
      }
      return write(atom);
    }
    case JSOP_GETLOCAL: {
      JSAtom* atom = FrameSlotName(script, pc);
      MOZ_ASSERT(atom);
      return write(atom);
    }
    case JSOP_GETALIASEDVAR: {
      JSAtom* atom = EnvironmentCoordinateNameSlow(script, pc);
      MOZ_ASSERT(atom);
      return write(atom);
    }

    case JSOP_DELPROP:
    case JSOP_STRICTDELPROP:
    case JSOP_LENGTH:
    case JSOP_GETPROP:
    case JSOP_GETBOUNDNAME:
    case JSOP_CALLPROP: {
      bool hasDelete = op == JSOP_DELPROP || op == JSOP_STRICTDELPROP;
      RootedAtom prop(cx,
                      (op == JSOP_LENGTH) ? cx->names().length : loadAtom(pc));
      MOZ_ASSERT(prop);
      return (hasDelete ? write("(delete ") : true) &&
             decompilePCForStackOperand(pc, -1) &&
             (IsIdentifier(prop)
                  ? write(".") && quote(prop, '\0')
                  : write("[") && quote(prop, '\'') && write("]")) &&
             (hasDelete ? write(")") : true);
    }
    case JSOP_GETPROP_SUPER: {
      RootedAtom prop(cx, loadAtom(pc));
      return write("super.") && quote(prop, '\0');
    }
    case JSOP_SETELEM:
    case JSOP_STRICTSETELEM:
      // NOTE: We don't show the right hand side of the operation because
      // it's used in error messages like: "a[0] is not readable".
      //
      // We could though.
      return decompilePCForStackOperand(pc, -3) && write("[") &&
             decompilePCForStackOperand(pc, -2) && write("]");

    case JSOP_DELELEM:
    case JSOP_STRICTDELELEM:
    case JSOP_GETELEM:
    case JSOP_CALLELEM: {
      bool hasDelete = (op == JSOP_DELELEM || op == JSOP_STRICTDELELEM);
      return (hasDelete ? write("(delete ") : true) &&
             decompilePCForStackOperand(pc, -2) && write("[") &&
             decompilePCForStackOperand(pc, -1) && write("]") &&
             (hasDelete ? write(")") : true);
    }

    case JSOP_GETELEM_SUPER:
      return write("super[") && decompilePCForStackOperand(pc, -2) &&
             write("]");
    case JSOP_NULL:
      return write(js_null_str);
    case JSOP_TRUE:
      return write(js_true_str);
    case JSOP_FALSE:
      return write(js_false_str);
    case JSOP_ZERO:
    case JSOP_ONE:
    case JSOP_INT8:
    case JSOP_UINT16:
    case JSOP_UINT24:
    case JSOP_INT32:
      return sprinter.printf("%d", GetBytecodeInteger(pc));
    case JSOP_STRING:
      return quote(loadAtom(pc), '"');
    case JSOP_SYMBOL: {
      unsigned i = uint8_t(pc[1]);
      MOZ_ASSERT(i < JS::WellKnownSymbolLimit);
      if (i < JS::WellKnownSymbolLimit) {
        return write(cx->names().wellKnownSymbolDescriptions()[i]);
      }
      break;
    }
    case JSOP_UNDEFINED:
      return write(js_undefined_str);
    case JSOP_GLOBALTHIS:
      // |this| could convert to a very long object initialiser, so cite it by
      // its keyword name.
      return write(js_this_str);
    case JSOP_NEWTARGET:
      return write("new.target");
    case JSOP_CALL:
    case JSOP_CALL_IGNORES_RV:
    case JSOP_CALLITER:
    case JSOP_FUNCALL:
    case JSOP_FUNAPPLY:
      return decompilePCForStackOperand(pc, -int32_t(GET_ARGC(pc) + 2)) &&
             write("(...)");
    case JSOP_SPREADCALL:
      return decompilePCForStackOperand(pc, -3) && write("(...)");
    case JSOP_NEWARRAY:
      return write("[]");
    case JSOP_REGEXP: {
      RootedObject obj(cx, script->getObject(GET_UINT32_INDEX(pc)));
      JSString* str = obj->as<RegExpObject>().toString(cx);
      if (!str) {
        return false;
      }
      return write(str);
    }
    case JSOP_NEWARRAY_COPYONWRITE: {
      RootedObject obj(cx, script->getObject(GET_UINT32_INDEX(pc)));
      Handle<ArrayObject*> aobj = obj.as<ArrayObject>();
      if (!write("[")) {
        return false;
      }
      for (size_t i = 0; i < aobj->getDenseInitializedLength(); i++) {
        if (i > 0 && !write(", ")) {
          return false;
        }

        RootedValue v(cx, aobj->getDenseElement(i));
        MOZ_RELEASE_ASSERT(v.isPrimitive() && !v.isMagic());

        JSString* str = ValueToSource(cx, v);
        if (!str || !write(str)) {
          return false;
        }
      }
      return write("]");
    }
    case JSOP_OBJECT: {
      JSObject* obj = script->getObject(GET_UINT32_INDEX(pc));
      RootedValue objv(cx, ObjectValue(*obj));
      JSString* str = ValueToSource(cx, objv);
      if (!str) {
        return false;
      }
      return write(str);
    }
    case JSOP_VOID:
      return write("(void ") && decompilePCForStackOperand(pc, -1) &&
             write(")");

    case JSOP_SUPERCALL:
    case JSOP_SPREADSUPERCALL:
      return write("super(...)");
    case JSOP_SUPERFUN:
      return write("super");

    case JSOP_EVAL:
    case JSOP_SPREADEVAL:
    case JSOP_STRICTEVAL:
    case JSOP_STRICTSPREADEVAL:
      return write("eval(...)");

    case JSOP_NEW:
      return write("(new ") &&
             decompilePCForStackOperand(pc, -int32_t(GET_ARGC(pc) + 3)) &&
             write("(...))");

    case JSOP_SPREADNEW:
      return write("(new ") && decompilePCForStackOperand(pc, -4) &&
             write("(...))");

    case JSOP_TYPEOF:
    case JSOP_TYPEOFEXPR:
      return write("(typeof ") && decompilePCForStackOperand(pc, -1) &&
             write(")");

    case JSOP_INITELEM_ARRAY:
      return write("[...]");

    case JSOP_INITELEM_INC:
      if (defIndex == 0) {
        return write("[...]");
      }
      MOZ_ASSERT(defIndex == 1);
#ifdef DEBUG
      // INDEX won't be be exposed to error message.
      if (isStackDump) {
        return write("INDEX");
      }
#endif
      break;

    default:
      break;
  }

#ifdef DEBUG
  if (isStackDump) {
    // Special decompilation for stack dump.
    switch (op) {
      case JSOP_ARGUMENTS:
        return write("arguments");

      case JSOP_BINDGNAME:
        return write("GLOBAL");

      case JSOP_BINDNAME:
      case JSOP_BINDVAR:
        return write("ENV");

      case JSOP_CALLEE:
        return write("CALLEE");

      case JSOP_ENVCALLEE:
        return write("ENVCALLEE");

      case JSOP_CALLSITEOBJ:
        return write("OBJ");

      case JSOP_CLASSCONSTRUCTOR:
      case JSOP_DERIVEDCONSTRUCTOR:
        return write("CONSTRUCTOR");

      case JSOP_DOUBLE:
        return sprinter.printf(
            "%lf", script->getConst(GET_UINT32_INDEX(pc)).toDouble());

      case JSOP_EXCEPTION:
        return write("EXCEPTION");

      case JSOP_FINALLY:
        if (defIndex == 0) {
          return write("THROWING");
        }
        MOZ_ASSERT(defIndex == 1);
        return write("PC");

      case JSOP_GIMPLICITTHIS:
      case JSOP_FUNCTIONTHIS:
      case JSOP_IMPLICITTHIS:
        return write("THIS");

      case JSOP_FUNWITHPROTO:
        return write("FUN");

      case JSOP_GENERATOR:
        return write("GENERATOR");

      case JSOP_GETIMPORT:
        return write("VAL");

      case JSOP_GETRVAL:
        return write("RVAL");

      case JSOP_HOLE:
        return write("HOLE");

      case JSOP_ISGENCLOSING:
        // For stack dump, defIndex == 0 is not used.
        MOZ_ASSERT(defIndex == 1);
        return write("ISGENCLOSING");

      case JSOP_ISNOITER:
        // For stack dump, defIndex == 0 is not used.
        MOZ_ASSERT(defIndex == 1);
        return write("ISNOITER");

      case JSOP_IS_CONSTRUCTING:
        return write("JS_IS_CONSTRUCTING");

      case JSOP_ITER:
        return write("ITER");

      case JSOP_LAMBDA:
      case JSOP_LAMBDA_ARROW:
      case JSOP_TOASYNC:
      case JSOP_TOASYNCGEN:
        return write("FUN");

      case JSOP_TOASYNCITER:
        return write("ASYNCITER");

      case JSOP_MOREITER:
        // For stack dump, defIndex == 0 is not used.
        MOZ_ASSERT(defIndex == 1);
        return write("MOREITER");

      case JSOP_MUTATEPROTO:
        return write("SUCCEEDED");

      case JSOP_NEWINIT:
      case JSOP_NEWOBJECT:
      case JSOP_OBJWITHPROTO:
        return write("OBJ");

      case JSOP_OPTIMIZE_SPREADCALL:
        // For stack dump, defIndex == 0 is not used.
        MOZ_ASSERT(defIndex == 1);
        return write("OPTIMIZED");

      case JSOP_REST:
        return write("REST");

      case JSOP_RESUME:
        return write("RVAL");

      case JSOP_SUPERBASE:
        return write("HOMEOBJECTPROTO");

      case JSOP_TOID:
        return write("TOID(") && decompilePCForStackOperand(pc, -1) &&
               write(")");
      case JSOP_TOSTRING:
        return write("TOSTRING(") && decompilePCForStackOperand(pc, -1) &&
               write(")");

      case JSOP_UNINITIALIZED:
        return write("UNINITIALIZED");

      case JSOP_AWAIT:
      case JSOP_YIELD:
        // Printing "yield SOMETHING" is confusing since the operand doesn't
        // match to the syntax, since the stack operand for "yield 10" is
        // the result object, not 10.
        return write("RVAL");

      default:
        break;
    }
    return write("<unknown>");
  }
#endif /* DEBUG */

  return write("(intermediate value)");
}

bool ExpressionDecompiler::decompilePC(
    const OffsetAndDefIndex& offsetAndDefIndex) {
  if (offsetAndDefIndex.isSpecial()) {
#ifdef DEBUG
    if (isStackDump) {
      if (offsetAndDefIndex.isMerged()) {
        if (!write("merged<")) {
          return false;
        }
      } else if (offsetAndDefIndex.isIgnored()) {
        if (!write("ignored<")) {
          return false;
        }
      }

      if (!decompilePC(script->offsetToPC(offsetAndDefIndex.specialOffset()),
                       offsetAndDefIndex.specialDefIndex())) {
        return false;
      }

      if (!write(">")) {
        return false;
      }

      return true;
    }
#endif /* DEBUG */
    return write("(intermediate value)");
  }

  return decompilePC(script->offsetToPC(offsetAndDefIndex.offset()),
                     offsetAndDefIndex.defIndex());
}

bool ExpressionDecompiler::init() {
  cx->check(script);
  return sprinter.init();
}

bool ExpressionDecompiler::write(const char* s) { return sprinter.put(s); }

bool ExpressionDecompiler::write(JSString* str) {
  if (str == cx->names().dotThis) {
    return write("this");
  }
  return sprinter.putString(str);
}

bool ExpressionDecompiler::quote(JSString* s, char quote) {
  return QuoteString(&sprinter, s, quote);
}

JSAtom* ExpressionDecompiler::loadAtom(jsbytecode* pc) {
  return script->getAtom(pc);
}

JSAtom* ExpressionDecompiler::getArg(unsigned slot) {
  MOZ_ASSERT(script->functionNonDelazifying());
  MOZ_ASSERT(slot < script->numArgs());

  for (PositionalFormalParameterIter fi(script); fi; fi++) {
    if (fi.argumentSlot() == slot) {
      if (!fi.isDestructured()) {
        return fi.name();
      }

      // Destructured arguments have no single binding name.
      static const char destructuredParam[] = "(destructured parameter)";
      return Atomize(cx, destructuredParam, strlen(destructuredParam));
    }
  }

  MOZ_CRASH("No binding");
}

UniqueChars ExpressionDecompiler::getOutput() {
  ptrdiff_t len = sprinter.stringEnd() - sprinter.stringAt(0);
  auto res = cx->make_pod_array<char>(len + 1);
  if (!res) {
    return nullptr;
  }
  js_memcpy(res.get(), sprinter.stringAt(0), len);
  res[len] = 0;
  return res;
}

}  // anonymous namespace

#if defined(DEBUG) || defined(JS_JITSPEW)
static bool DecompileAtPCForStackDump(
    JSContext* cx, HandleScript script,
    const OffsetAndDefIndex& offsetAndDefIndex, Sprinter* sp) {
  // The expression decompiler asserts the script is in the current realm.
  AutoRealm ar(cx, script);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  BytecodeParser parser(cx, allocScope.alloc(), script);
  parser.setStackDump();
  if (!parser.parse()) {
    return false;
  }

  ExpressionDecompiler ed(cx, script, parser);
  ed.setStackDump();
  if (!ed.init()) {
    return false;
  }

  if (!ed.decompilePC(offsetAndDefIndex)) {
    return false;
  }

  UniqueChars result = ed.getOutput();
  if (!result) {
    return false;
  }

  return sp->put(result.get());
}
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

static bool FindStartPC(JSContext* cx, const FrameIter& iter,
                        const BytecodeParser& parser, int spindex,
                        int skipStackHits, const Value& v, jsbytecode** valuepc,
                        uint8_t* defIndex) {
  jsbytecode* current = *valuepc;
  *valuepc = nullptr;
  *defIndex = 0;

  if (spindex < 0 && spindex + int(parser.stackDepthAtPC(current)) < 0) {
    spindex = JSDVG_SEARCH_STACK;
  }

  if (spindex == JSDVG_SEARCH_STACK) {
    size_t index = iter.numFrameSlots();

    // The decompiler may be called from inside functions that are not
    // called from script, but via the C++ API directly, such as
    // Invoke. In that case, the youngest script frame may have a
    // completely unrelated pc and stack depth, so we give up.
    if (index < size_t(parser.stackDepthAtPC(current))) {
      return true;
    }

    // We search from fp->sp to base to find the most recently calculated
    // value matching v under assumption that it is the value that caused
    // the exception.
    int stackHits = 0;
    Value s;
    do {
      if (!index) {
        return true;
      }
      s = iter.frameSlotValue(--index);
    } while (s != v || stackHits++ != skipStackHits);

    // If the current PC has fewer values on the stack than the index we are
    // looking for, the blamed value must be one pushed by the current
    // bytecode (e.g. JSOP_MOREITER), so restore *valuepc.
    if (index < size_t(parser.stackDepthAtPC(current))) {
      *valuepc = parser.pcForStackOperand(current, index, defIndex);
    } else {
      *valuepc = current;
      *defIndex = index - size_t(parser.stackDepthAtPC(current));
    }
  } else {
    *valuepc = parser.pcForStackOperand(current, spindex, defIndex);
  }
  return true;
}

static bool DecompileExpressionFromStack(JSContext* cx, int spindex,
                                         int skipStackHits, HandleValue v,
                                         UniqueChars* res) {
  MOZ_ASSERT(spindex < 0 || spindex == JSDVG_IGNORE_STACK ||
             spindex == JSDVG_SEARCH_STACK);

  *res = nullptr;

#ifdef JS_MORE_DETERMINISTIC
  /*
   * Give up if we need deterministic behavior for differential testing.
   * IonMonkey doesn't use InterpreterFrames and this ensures we get the same
   * error messages.
   */
  return true;
#endif

  if (spindex == JSDVG_IGNORE_STACK) {
    return true;
  }

  FrameIter frameIter(cx);

  if (frameIter.done() || !frameIter.hasScript() ||
      frameIter.realm() != cx->realm()) {
    return true;
  }

  /*
   * FIXME: Fall back if iter.isIon(), since the stack snapshot may be for the
   * previous pc (see bug 831120).
   */
  if (frameIter.isIon()) {
    return true;
  }

  RootedScript script(cx, frameIter.script());
  jsbytecode* valuepc = frameIter.pc();

  MOZ_ASSERT(script->containsPC(valuepc));

  // Give up if in prologue.
  if (valuepc < script->main()) {
    return true;
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  BytecodeParser parser(cx, allocScope.alloc(), frameIter.script());
  if (!parser.parse()) {
    return false;
  }

  uint8_t defIndex;
  if (!FindStartPC(cx, frameIter, parser, spindex, skipStackHits, v, &valuepc,
                   &defIndex)) {
    return false;
  }
  if (!valuepc) {
    return true;
  }

  ExpressionDecompiler ed(cx, script, parser);
  if (!ed.init()) {
    return false;
  }
  if (!ed.decompilePC(valuepc, defIndex)) {
    return false;
  }

  *res = ed.getOutput();
  return *res != nullptr;
}

UniqueChars js::DecompileValueGenerator(JSContext* cx, int spindex,
                                        HandleValue v, HandleString fallbackArg,
                                        int skipStackHits) {
  RootedString fallback(cx, fallbackArg);
  {
    UniqueChars result;
    if (!DecompileExpressionFromStack(cx, spindex, skipStackHits, v, &result)) {
      return nullptr;
    }
    if (result && strcmp(result.get(), "(intermediate value)")) {
      return result;
    }
  }
  if (!fallback) {
    if (v.isUndefined()) {
      return DuplicateString(
          cx, js_undefined_str);  // Prevent users from seeing "(void 0)"
    }
    fallback = ValueToSource(cx, v);
    if (!fallback) {
      return nullptr;
    }
  }

  return StringToNewUTF8CharsZ(cx, *fallback);
}

static bool DecompileArgumentFromStack(JSContext* cx, int formalIndex,
                                       UniqueChars* res) {
  MOZ_ASSERT(formalIndex >= 0);

  *res = nullptr;

#ifdef JS_MORE_DETERMINISTIC
  /* See note in DecompileExpressionFromStack. */
  return true;
#endif

  /*
   * Settle on the nearest script frame, which should be the builtin that
   * called the intrinsic.
   */
  FrameIter frameIter(cx);
  MOZ_ASSERT(!frameIter.done());
  MOZ_ASSERT(frameIter.script()->selfHosted());

  /*
   * Get the second-to-top frame, the non-self-hosted caller of the builtin
   * that called the intrinsic.
   */
  ++frameIter;
  if (frameIter.done() || !frameIter.hasScript() ||
      frameIter.script()->selfHosted() || frameIter.realm() != cx->realm()) {
    return true;
  }

  RootedScript script(cx, frameIter.script());
  jsbytecode* current = frameIter.pc();

  MOZ_ASSERT(script->containsPC(current));

  if (current < script->main()) {
    return true;
  }

  /* Don't handle getters, setters or calls from fun.call/fun.apply. */
  JSOp op = JSOp(*current);
  if (op != JSOP_CALL && op != JSOP_CALL_IGNORES_RV && op != JSOP_NEW) {
    return true;
  }

  if (static_cast<unsigned>(formalIndex) >= GET_ARGC(current)) {
    return true;
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  BytecodeParser parser(cx, allocScope.alloc(), script);
  if (!parser.parse()) {
    return false;
  }

  bool pushedNewTarget = op == JSOP_NEW;
  int formalStackIndex = parser.stackDepthAtPC(current) - GET_ARGC(current) -
                         pushedNewTarget + formalIndex;
  MOZ_ASSERT(formalStackIndex >= 0);
  if (uint32_t(formalStackIndex) >= parser.stackDepthAtPC(current)) {
    return true;
  }

  ExpressionDecompiler ed(cx, script, parser);
  if (!ed.init()) {
    return false;
  }
  if (!ed.decompilePCForStackOperand(current, formalStackIndex)) {
    return false;
  }

  *res = ed.getOutput();
  return *res != nullptr;
}

JSString* js::DecompileArgument(JSContext* cx, int formalIndex, HandleValue v) {
  {
    UniqueChars result;
    if (!DecompileArgumentFromStack(cx, formalIndex, &result)) {
      return nullptr;
    }
    if (result && strcmp(result.get(), "(intermediate value)")) {
      JS::ConstUTF8CharsZ utf8chars(result.get(), strlen(result.get()));
      return NewStringCopyUTF8Z<CanGC>(cx, utf8chars);
    }
  }
  if (v.isUndefined()) {
    return cx->names().undefined;  // Prevent users from seeing "(void 0)"
  }

  return ValueToSource(cx, v);
}

extern bool js::IsValidBytecodeOffset(JSContext* cx, JSScript* script,
                                      size_t offset) {
  // This could be faster (by following jump instructions if the target
  // is <= offset).
  for (BytecodeRange r(cx, script); !r.empty(); r.popFront()) {
    size_t here = r.frontOffset();
    if (here >= offset) {
      return here == offset;
    }
  }
  return false;
}

/*
 * There are three possible PCCount profiling states:
 *
 * 1. None: Neither scripts nor the runtime have count information.
 * 2. Profile: Active scripts have count information, the runtime does not.
 * 3. Query: Scripts do not have count information, the runtime does.
 *
 * When starting to profile scripts, counting begins immediately, with all JIT
 * code discarded and recompiled with counts as necessary. Active interpreter
 * frames will not begin profiling until they begin executing another script
 * (via a call or return).
 *
 * The below API functions manage transitions to new states, according
 * to the table below.
 *
 *                                  Old State
 *                          -------------------------
 * Function                 None      Profile   Query
 * --------
 * StartPCCountProfiling    Profile   Profile   Profile
 * StopPCCountProfiling     None      Query     Query
 * PurgePCCounts            None      None      None
 */

static void ReleaseScriptCounts(FreeOp* fop) {
  JSRuntime* rt = fop->runtime();
  MOZ_ASSERT(rt->scriptAndCountsVector);

  fop->delete_(rt->scriptAndCountsVector.ref());
  rt->scriptAndCountsVector = nullptr;
}

JS_FRIEND_API void js::StartPCCountProfiling(JSContext* cx) {
  JSRuntime* rt = cx->runtime();

  if (rt->profilingScripts) {
    return;
  }

  if (rt->scriptAndCountsVector) {
    ReleaseScriptCounts(rt->defaultFreeOp());
  }

  ReleaseAllJITCode(rt->defaultFreeOp());

  rt->profilingScripts = true;
}

JS_FRIEND_API void js::StopPCCountProfiling(JSContext* cx) {
  JSRuntime* rt = cx->runtime();

  if (!rt->profilingScripts) {
    return;
  }
  MOZ_ASSERT(!rt->scriptAndCountsVector);

  ReleaseAllJITCode(rt->defaultFreeOp());

  auto* vec = cx->new_<PersistentRooted<ScriptAndCountsVector>>(
      cx, ScriptAndCountsVector(SystemAllocPolicy()));
  if (!vec) {
    return;
  }

  for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
    for (auto iter = zone->cellIter<JSScript>(); !iter.done(); iter.next()) {
      JSScript* script = iter;
      if (gc::IsAboutToBeFinalizedUnbarriered(&script)) {
        continue;
      }
      AutoSweepTypeScript sweep(script);
      if (script->hasScriptCounts() && script->types(sweep)) {
        if (!vec->append(script)) {
          return;
        }
      }
    }
  }

  rt->profilingScripts = false;
  rt->scriptAndCountsVector = vec;
}

JS_FRIEND_API void js::PurgePCCounts(JSContext* cx) {
  JSRuntime* rt = cx->runtime();

  if (!rt->scriptAndCountsVector) {
    return;
  }
  MOZ_ASSERT(!rt->profilingScripts);

  ReleaseScriptCounts(rt->defaultFreeOp());
}

JS_FRIEND_API size_t js::GetPCCountScriptCount(JSContext* cx) {
  JSRuntime* rt = cx->runtime();

  if (!rt->scriptAndCountsVector) {
    return 0;
  }

  return rt->scriptAndCountsVector->length();
}

static MOZ_MUST_USE bool JSONStringProperty(Sprinter& sp, JSONPrinter& json,
                                            const char* name, JSString* str) {
  json.beginStringProperty(name);
  if (!JSONQuoteString(&sp, str)) {
    return false;
  }
  json.endStringProperty();
  return true;
}

JS_FRIEND_API JSString* js::GetPCCountScriptSummary(JSContext* cx,
                                                    size_t index) {
  JSRuntime* rt = cx->runtime();

  if (!rt->scriptAndCountsVector ||
      index >= rt->scriptAndCountsVector->length()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BUFFER_TOO_SMALL);
    return nullptr;
  }

  const ScriptAndCounts& sac = (*rt->scriptAndCountsVector)[index];
  RootedScript script(cx, sac.script);

  Sprinter sp(cx);
  if (!sp.init()) {
    return nullptr;
  }

  JSONPrinter json(sp, false);

  json.beginObject();

  RootedString filename(cx, NewStringCopyZ<CanGC>(cx, script->filename()));
  if (!filename) {
    return nullptr;
  }
  if (!JSONStringProperty(sp, json, "file", filename)) {
    return nullptr;
  }
  json.property("line", script->lineno());

  if (JSFunction* fun = script->functionNonDelazifying()) {
    if (JSAtom* atom = fun->displayAtom()) {
      if (!JSONStringProperty(sp, json, "name", atom)) {
        return nullptr;
      }
    }
  }

  uint64_t total = 0;

  jsbytecode* codeEnd = script->codeEnd();
  for (jsbytecode* pc = script->code(); pc < codeEnd; pc = GetNextPc(pc)) {
    if (const PCCounts* counts = sac.maybeGetPCCounts(pc)) {
      total += counts->numExec();
    }
  }

  json.beginObjectProperty("totals");

  json.property(PCCounts::numExecName, total);

  uint64_t ionActivity = 0;
  jit::IonScriptCounts* ionCounts = sac.getIonCounts();
  while (ionCounts) {
    for (size_t i = 0; i < ionCounts->numBlocks(); i++) {
      ionActivity += ionCounts->block(i).hitCount();
    }
    ionCounts = ionCounts->previous();
  }
  if (ionActivity) {
    json.property("ion", ionActivity);
  }

  json.endObject();

  json.endObject();

  if (sp.hadOutOfMemory()) {
    return nullptr;
  }

  return NewStringCopyZ<CanGC>(cx, sp.string());
}

static bool GetPCCountJSON(JSContext* cx, const ScriptAndCounts& sac,
                           Sprinter& sp) {
  JSONPrinter json(sp, false);

  RootedScript script(cx, sac.script);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  BytecodeParser parser(cx, allocScope.alloc(), script);
  if (!parser.parse()) {
    return false;
  }

  json.beginObject();

  JSString* str = JS_DecompileScript(cx, script);
  if (!str) {
    return false;
  }

  if (!JSONStringProperty(sp, json, "text", str)) {
    return false;
  }

  json.property("line", script->lineno());

  json.beginListProperty("opcodes");

  uint64_t hits = 0;
  for (BytecodeRangeWithPosition range(cx, script); !range.empty();
       range.popFront()) {
    jsbytecode* pc = range.frontPC();
    size_t offset = script->pcToOffset(pc);
    JSOp op = JSOp(*pc);

    // If the current instruction is a jump target,
    // then update the number of hits.
    if (const PCCounts* counts = sac.maybeGetPCCounts(pc)) {
      hits = counts->numExec();
    }

    json.beginObject();

    json.property("id", offset);
    json.property("line", range.frontLineNumber());
    json.property("name", CodeName[op]);

    {
      ExpressionDecompiler ed(cx, script, parser);
      if (!ed.init()) {
        return false;
      }
      // defIndex passed here is not used.
      if (!ed.decompilePC(pc, /* defIndex = */ 0)) {
        return false;
      }
      UniqueChars text = ed.getOutput();
      if (!text) {
        return false;
      }

      JS::ConstUTF8CharsZ utf8chars(text.get(), strlen(text.get()));
      JSString* str = NewStringCopyUTF8Z<CanGC>(cx, utf8chars);
      if (!str) {
        return false;
      }

      if (!JSONStringProperty(sp, json, "text", str)) {
        return false;
      }
    }

    json.beginObjectProperty("counts");
    if (hits > 0) {
      json.property(PCCounts::numExecName, hits);
    }
    json.endObject();

    json.endObject();

    // If the current instruction has thrown,
    // then decrement the hit counts with the number of throws.
    if (const PCCounts* counts = sac.maybeGetThrowCounts(pc)) {
      hits -= counts->numExec();
    }
  }

  json.endList();

  if (jit::IonScriptCounts* ionCounts = sac.getIonCounts()) {
    json.beginListProperty("ion");

    while (ionCounts) {
      json.beginList();
      for (size_t i = 0; i < ionCounts->numBlocks(); i++) {
        const jit::IonBlockCounts& block = ionCounts->block(i);

        json.beginObject();
        json.property("id", block.id());
        json.property("offset", block.offset());

        json.beginListProperty("successors");
        for (size_t j = 0; j < block.numSuccessors(); j++) {
          json.value(block.successor(j));
        }
        json.endList();

        json.property("hits", block.hitCount());

        JSString* str = NewStringCopyZ<CanGC>(cx, block.code());
        if (!str) {
          return false;
        }

        if (!JSONStringProperty(sp, json, "code", str)) {
          return false;
        }

        json.endObject();
      }
      json.endList();

      ionCounts = ionCounts->previous();
    }

    json.endList();
  }

  json.endObject();

  return true;
}

JS_FRIEND_API JSString* js::GetPCCountScriptContents(JSContext* cx,
                                                     size_t index) {
  JSRuntime* rt = cx->runtime();

  if (!rt->scriptAndCountsVector ||
      index >= rt->scriptAndCountsVector->length()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BUFFER_TOO_SMALL);
    return nullptr;
  }

  const ScriptAndCounts& sac = (*rt->scriptAndCountsVector)[index];
  JSScript* script = sac.script;

  Sprinter sp(cx);
  if (!sp.init()) {
    return nullptr;
  }

  {
    AutoRealm ar(cx, &script->global());
    if (!GetPCCountJSON(cx, sac, sp)) {
      return nullptr;
    }
  }

  if (sp.hadOutOfMemory()) {
    return nullptr;
  }

  return NewStringCopyZ<CanGC>(cx, sp.string());
}

static bool GenerateLcovInfo(JSContext* cx, JS::Realm* realm,
                             GenericPrinter& out) {
  JSRuntime* rt = cx->runtime();

  // Collect the list of scripts which are part of the current realm.
  { js::gc::AutoPrepareForTracing apft(cx); }

  // Hold the scripts that we have already flushed, to avoid flushing them
  // twice.
  using JSScriptSet = GCHashSet<JSScript*>;
  Rooted<JSScriptSet> scriptsDone(cx, JSScriptSet(cx));

  Rooted<ScriptVector> queue(cx, ScriptVector(cx));
  for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
    for (auto iter = zone->cellIter<JSScript>(); !iter.done(); iter.next()) {
      JSScript* script = iter;
      if (gc::IsAboutToBeFinalizedUnbarriered(&script)) {
        continue;
      }
      if (script->realm() != realm || !script->filename()) {
        continue;
      }

      if (!queue.append(script)) {
        return false;
      }
    }
  }

  if (queue.length() == 0) {
    return true;
  }

  // Collect code coverage info for one realm.
  do {
    RootedScript script(cx, queue.popCopy());
    RootedFunction fun(cx);

    JSScriptSet::AddPtr entry = scriptsDone.lookupForAdd(script);
    if (script->filename() && !entry) {
      realm->lcovOutput.collectCodeCoverageInfo(realm, script,
                                                script->filename());
      script->resetScriptCounts();

      if (!scriptsDone.add(entry, script)) {
        return false;
      }
    }

    if (!script->isTopLevel()) {
      continue;
    }

    // Iterate from the last to the first object in order to have
    // the functions them visited in the opposite order when popping
    // elements from the stack of remaining scripts, such that the
    // functions are more-less listed with increasing line numbers.
    if (!script->hasObjects()) {
      continue;
    }
    auto objects = script->objects();
    for (JSObject* obj : mozilla::Reversed(objects)) {
      // Only continue on JSFunction objects.
      if (!obj->is<JSFunction>()) {
        continue;
      }
      fun = &obj->as<JSFunction>();

      // Let's skip wasm for now.
      if (!fun->isInterpreted()) {
        continue;
      }

      // Queue the script in the list of script associated to the
      // current source.
      JSScript* childScript = JSFunction::getOrCreateScript(cx, fun);
      if (!childScript || !queue.append(childScript)) {
        return false;
      }
    }
  } while (!queue.empty());

  bool isEmpty = true;
  realm->lcovOutput.exportInto(out, &isEmpty);
  if (out.hadOutOfMemory()) {
    return false;
  }

  return true;
}

JS_FRIEND_API char* js::GetCodeCoverageSummary(JSContext* cx, size_t* length) {
  Sprinter out(cx);

  if (!out.init()) {
    return nullptr;
  }

  for (RealmsIter realm(cx->runtime()); !realm.done(); realm.next()) {
    if (!GenerateLcovInfo(cx, realm, out)) {
      JS_ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  if (out.hadOutOfMemory()) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  ptrdiff_t len = out.stringEnd() - out.string();
  char* res = cx->pod_malloc<char>(len + 1);
  if (!res) {
    return nullptr;
  }

  js_memcpy(res, out.string(), len);
  res[len] = 0;
  if (length) {
    *length = len;
  }
  return res;
}

bool js::GetSuccessorBytecodes(JSScript* script, jsbytecode* pc,
                               PcVector& successors) {
  MOZ_ASSERT(script->containsPC(pc));

  JSOp op = (JSOp)*pc;
  if (FlowsIntoNext(op)) {
    if (!successors.append(GetNextPc(pc))) {
      return false;
    }
  }

  if (CodeSpec[op].type() == JOF_JUMP) {
    if (!successors.append(pc + GET_JUMP_OFFSET(pc))) {
      return false;
    }
  } else if (op == JSOP_TABLESWITCH) {
    if (!successors.append(pc + GET_JUMP_OFFSET(pc))) {
      return false;
    }
    jsbytecode* npc = pc + JUMP_OFFSET_LEN;

    int32_t low = GET_JUMP_OFFSET(npc);
    npc += JUMP_OFFSET_LEN;
    int ncases = GET_JUMP_OFFSET(npc) - low + 1;
    npc += JUMP_OFFSET_LEN;

    for (int i = 0; i < ncases; i++) {
      if (!successors.append(script->tableSwitchCasePC(pc, i))) {
        return false;
      }
    }
  }

  return true;
}

bool js::GetPredecessorBytecodes(JSScript* script, jsbytecode* pc,
                                 PcVector& predecessors) {
  jsbytecode* end = script->code() + script->length();
  MOZ_ASSERT(pc >= script->code() && pc < end);
  for (jsbytecode* npc = script->code(); npc < end; npc = GetNextPc(npc)) {
    PcVector successors;
    if (!GetSuccessorBytecodes(script, npc, successors)) {
      return false;
    }
    for (size_t i = 0; i < successors.length(); i++) {
      if (successors[i] == pc) {
        if (!predecessors.append(npc)) {
          return false;
        }
        break;
      }
    }
  }
  return true;
}
