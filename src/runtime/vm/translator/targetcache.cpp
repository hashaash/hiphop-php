/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include <string>
#include <stdio.h>
#include <sys/mman.h>

#include <util/trace.h>
#include <util/base.h>
#include <runtime/base/complex_types.h>
#include <runtime/base/execution_context.h>
#include <runtime/base/types.h>
#include <runtime/base/tv_macros.h>
#include <runtime/base/strings.h>
#include <runtime/vm/unit.h>
#include <runtime/vm/class.h>
#include <runtime/vm/runtime.h>
#include <runtime/vm/translator/annotation.h>
#include <runtime/vm/translator/targetcache.h>
#include <runtime/vm/translator/translator-inline.h>
#include <runtime/vm/exception_gate.h>
#include <system/gen/sys/system_globals.h>
#include <runtime/vm/stats.h>

using namespace HPHP::MethodLookup;
using namespace HPHP::Util;
using std::string;

/*
 * The targetcache module provides a set of per-request caches.
 */
namespace HPHP {
namespace VM {
namespace Transl {
namespace TargetCache {

TRACE_SET_MOD(targetcache);

static StaticString s___call(LITSTR_INIT("__call"));

// Shorthand.
typedef CacheHandle Handle;

// Helper for lookup failures. msg should be a printf-style static
// format with one %s parameter, which name will be substituted into.
void
undefinedError(const char* msg, const char* name) {
  VMRegAnchor _;
  EXCEPTION_GATE_ENTER();
  raise_error(msg, name);
  EXCEPTION_GATE_RETURN();
}

// Targetcache memory. See the comment in targetcache.h
__thread HPHP::x64::DataBlock tl_targetCaches = {0, 0, kNumTargetCacheBytes};
size_t s_frontier;
static size_t s_next_bit;
static size_t s_bits_to_go;

// Mapping from names to targetcache locations. Protected by the translator
// write lease.
typedef hphp_hash_map<const StringData*, Handle, string_data_hash,
        string_data_isame>
  HandleMapIS;

typedef hphp_hash_map<const StringData*, Handle, string_data_hash,
        string_data_same>
  HandleMapCS;

// handleMaps[NSConstant]['FOO'] is the cache associated with the constant
// FOO, eg. handleMaps is a rare instance of shared, mutable state across
// the request threads in the translator: it is essentially a lazily
// constructed link table for tl_targetCaches.
HandleMapIS handleMapsIS[NumInsensitive];
HandleMapCS handleMapsCS[NumCaseSensitive];

// Vector of cache handles
typedef std::vector<Handle> HandleVector;

// Set of FuncCache handles for dynamic function callsites, used for
// invalidation when a function is renamed.
HandleVector funcCacheEntries;
// Set of CallCache handles for dynamic function callsites, used for
// invalidation when a function is renamed or intercepted.
HandleVector callCacheEntries;

// RAII lock for handleMaps/funcCacheEntries. Allow recursive acquisitions.
class HandleMutex {
  static pthread_mutex_t m_lock;
 public:
  HandleMutex() {
    pthread_mutex_lock(&m_lock);
  }
  ~HandleMutex() {
    pthread_mutex_unlock(&m_lock);
  }
};
pthread_mutex_t HandleMutex::m_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

inline Handle
ptrToHandle(const void* ptr) {
  ptrdiff_t retval = uintptr_t(ptr) - uintptr_t(tl_targetCaches.base);
  ASSERT(retval < kNumTargetCacheBytes);
  return retval;
}

template <bool sensitive>
class HandleInfo {
public:
  typedef HandleMapIS Map;
  static Map &getHandleMap(int where) {
    return handleMapsIS[where];
  }
};

template <>
class HandleInfo<true> {
public:
  typedef HandleMapCS Map;
  static Map &getHandleMap(int where) {
    return handleMapsCS[where - FirstCaseSensitive];
  }
};

#define getHMap(where) \
  HandleInfo<where >= FirstCaseSensitive>::getHandleMap(where)

static size_t allocBitImpl(const StringData* name, PHPNameSpace ns) {
  ASSERT_NOT_IMPLEMENTED(ns == NSInvalid || ns >= FirstCaseSensitive);
  HandleMutex mtx;
  HandleMapCS& map = HandleInfo<true>::getHandleMap(ns);
  Handle handle;
  if (name != NULL && ns != NSInvalid && mapGet(map, name, &handle)) {
    return handle;
  }
  if (!s_bits_to_go) {
    s_next_bit = s_frontier * CHAR_BIT;
    // allocate at least 64 bytes, and make sure we end
    // on a 64 byte aligned boundary.
    int bytes = ((~s_frontier + 1) & 63) + 64;
    s_bits_to_go = bytes * CHAR_BIT;
    s_frontier += bytes;
  }
  s_bits_to_go--;
  if (name != NULL && ns != NSInvalid) {
    if (!name->isStatic()) name = StringData::GetStaticString(name);
    mapInsertUnique(map, name, s_next_bit);
  }
  return s_next_bit++;
}

size_t allocBit() {
  return allocBitImpl(NULL, NSInvalid);
}

size_t allocCnsBit(const StringData* name) {
  return allocBitImpl(name, NSCnsBits);
}

Handle bitOffToHandleAndMask(size_t bit, uint32 &mask) {
  CT_ASSERT(!(32 % CHAR_BIT));
  mask = (uint32)1 << (bit % 32);
  size_t off = bit / CHAR_BIT;
  off -= off % (32 / CHAR_BIT);
  return off;
}

bool testBit(size_t bit) {
  Handle handle = bit / CHAR_BIT;
  unsigned char mask = 1 << (bit % CHAR_BIT);
  return *(unsigned char*)handleToPtr(handle) & mask;
}

bool testBit(Handle handle, uint32 mask) {
  ASSERT(!(mask & (mask - 1)));
  return *(uint32*)handleToPtr(handle) & mask;
}

bool testAndSetBit(size_t bit) {
  Handle handle = bit / CHAR_BIT;
  unsigned char mask = 1 << (bit % CHAR_BIT);
  bool ret = *(unsigned char*)handleToPtr(handle) & mask;
  *(unsigned char*)handleToPtr(handle) |= mask;
  return ret;
}

bool testAndSetBit(Handle handle, uint32 mask) {
  ASSERT(!(mask & (mask - 1)));
  bool ret = *(uint32*)handleToPtr(handle) & mask;
  *(uint32*)handleToPtr(handle) |= mask;
  return ret;
}

// namedAlloc --
//   Many targetcache entries (Func, Class, Constant, ...) have
//   request-unique values. There is no reason to allocate more than
//   one item for all such calls in a request.
//
//   handleMaps acts as a de-facto dynamic link table that lives
//   across requests; the translator can write out code that assumes
//   that a given named entity's location in tl_targetCaches is
//   stable from request to request.
template<bool sensitive>
Handle
namedAlloc(PHPNameSpace where, const StringData* name,
           int numBytes, int align) {
  ASSERT(!name || (where >= 0 && where < NumNameSpaces));
  Handle retval;
  HandleMutex mtx;
  typedef HandleInfo<sensitive> HI;
  typename HI::Map& map = HI::getHandleMap(where);
  if (name && mapGet(map, name, &retval)) {
    TRACE(2, "TargetCache: hit \"%s\", %d\n", name->data(), int(retval));
    return retval;
  }
  void *mem = tl_targetCaches.allocAt(s_frontier, numBytes, align);
  retval = ptrToHandle(mem);
  if (name) {
    if (!name->isStatic()) name = StringData::GetStaticString(name);
    mapInsertUnique(map, name, retval);
    TRACE(1, "TargetCache: inserted \"%s\", %d\n", name->data(), int(retval));
  } else if (where == NSDynFunction) {
    funcCacheEntries.push_back(retval);
  } else if (where == NSFuncToTCA) {
    callCacheEntries.push_back(retval);
  }
  return retval;
}

void
invalidateForRename(const StringData* name) {
  ASSERT(name);
  HandleMutex mtx;
  {
    Handle handle;
    HandleMapIS& map = getHMap(NSFunction);
    if (mapGet(map, name, &handle)) {
      TRACE(1, "TargetCaches: invalidating mapping for NSFunction::%s\n",
            name->data());
      // OK, there's a targetcache for this name.
      FixedFuncCache::invalidate(handle);
    }
  }

  for (HandleVector::iterator i = funcCacheEntries.begin();
       i != funcCacheEntries.end(); ++i) {
    FuncCache::invalidate(*i, name);
  }
}

// requestInit --
//   Per-request work.
void
requestInit() {
  TRACE(1, "TargetCaches: @%p\n", tl_targetCaches.base);
  if (!tl_targetCaches.base) {
    tl_targetCaches.init();
    // allocate a bit so that target cache offsets don't start at 0
    allocBit();
  }
  TRACE(1, "TargetCaches: bzeroing %zd bytes: %p\n", s_frontier,
        tl_targetCaches.base);
  if (madvise(tl_targetCaches.base, s_frontier, MADV_DONTNEED) < 0) {
    not_reached();
  }
}

static inline bool
stringMatches(const StringData* rowString, const StringData* sd) {
  return rowString &&
    (rowString == sd ||
     rowString->data() == sd->data() ||
     (rowString->hash() == sd->hash() &&
      rowString->same(sd)));

}

//=============================================================================
// FuncCache
template<>
inline int
FuncCache::hashKey(const StringData* sd) {
  return sd->hash();
}

template<>
const Func*
FuncCache::lookup(Handle handle, StringData *sd, const void* /* ignored */) {
  FuncCache* thiz = cacheAtHandle(handle);
  Func* func;
  Pair* pair = thiz->keyToPair(sd);
  const StringData* pairSd = pair->m_key;
  if (!stringMatches(pairSd, sd)) {
    // Miss. Does it actually exist?
    func = Unit::lookupFunc(sd);
    if (UNLIKELY(!func)) {
      undefinedError("Undefined function: %s", sd->data());
    }
    func->validate();
    pair->m_key = func->name(); // use a static name
    pair->m_value = func;
  }
  // DecRef the string here; more compact than doing so in
  // callers.
  if (sd->decRefCount() == 0) {
    sd->release();
  }
  ASSERT(stringMatches(pair->m_key, pair->m_value->name()));
  pair->m_value->validate();
  return pair->m_value;
}

//=============================================================================
// FixedFuncCache

void FixedFuncCache::lookupFailed(StringData* name) {
  undefinedError("Undefined function: %s", name->data());
}

//=============================================================================
// MethodCache

template<>
inline int
MethodCache::hashKey(const Class* c) {
  pointer_hash<Class> h;
  return h(c);
}

template<>
HOT_FUNC_VM
MethodCacheEntry
MethodCache::lookup(Handle handle, ActRec *ar, const void* extraKey) {
  StringData* name = (StringData*)extraKey;
  ASSERT(ar->hasThis());
  ObjectData* obj = ar->getThis();
  Class* c = obj->getVMClass();
  ASSERT(c);
  MethodCache* thiz = MethodCache::cacheAtHandle(handle);
  Pair* pair = thiz->keyToPair(c);
  const Func* func = NULL;
  bool isMagicCall = false;
  if (pair->m_key == c) {
    func = pair->m_value.getFunc();
    ASSERT(func);
    isMagicCall = pair->m_value.isMagicCall();
    Stats::inc(Stats::TgtCache_MethodHit);
  } else {
    ASSERT(IMPLIES(pair->m_key, pair->m_value.getFunc()));
    if (pair->m_key &&
        (func = c->wouldCall(pair->m_value.getFunc())) != NULL) {
      if (UNLIKELY(pair->m_value.isMagicCall())) {
        /*
         * Don't accept another class's __call method as evidence that we
         * don't have a definition for this method.
         */
        func = NULL;
      }
      // Leave isMagicCall false
    }
    Stats::inc(Stats::TgtCache_MethodHit, func != NULL);
    if (!func) {
      // lookupObjMethod uses the current frame pointer to resolve context,
      // so we'd better sync regs.
      Class* ctx = arGetContextClass((ActRec*)ar->m_savedRbp);
      Stats::inc(Stats::TgtCache_MethodMiss);
      TRACE(2, "MethodCache: miss class %p name %s!\n", c, name->data());
      func = g_vmContext->lookupMethodCtx(c, name, ctx, ObjMethod, false);
      if (UNLIKELY(!func)) {
        isMagicCall = true;
        func = g_vmContext->lookupMethodCtx(c, s___call.get(), ctx, ObjMethod,
                                            false);
        if (UNLIKELY(!func)) {
          // Do it again, but raise the error this time. Keeps the VMRegAnchor
          // off the hot path; this should be wildly unusual, since we're
          // probably about to fatal.
          VMRegAnchor _;
          EXCEPTION_GATE_ENTER();
          (void) g_vmContext->lookupObjMethod(func, c, name, true);
          EXCEPTION_GATE_LEAVE();
          // Yet somehow we succeeded. private __call? Keep going.
        }
      }
    }
    pair->m_value.set(func, isMagicCall);
    pair->m_key = c;
  }
  ASSERT(func);
  func->validate();

  ar->m_func = func;
  if (UNLIKELY(func->attrs() & AttrStatic)) {
    // Drop the ActRec's reference to the current instance
    if (obj->decRefCount() == 0) {
      obj->release();
    }
    if (debug) ar->setThis(NULL); // suppress ASSERT in setClass
    // Set the ActRec's class (needed for late static binding)
    ar->setClass(c);
  }
  ASSERT(!ar->hasVarEnv() && !ar->hasInvName());
  if (UNLIKELY(isMagicCall)) {
    ar->setInvName(name);
    name->incRefCount();
  }
  return pair->m_value;
}

//=============================================================================
// GlobalCache
//  | - BoxedGlobalCache

static inline HphpArray*
getGlobArray() {
  SystemGlobals *g = (SystemGlobals*)get_global_variables();
  return
    dynamic_cast<HphpArray*>(g->hg_global_storage.getArrayData());
}

template<bool isBoxed>
inline TypedValue*
GlobalCache::lookupImpl(StringData *name, bool allowCreate) {
  bool hit ATTRIBUTE_UNUSED;
  if (UNLIKELY(m_globals == NULL)) {
    m_globals = getGlobArray();
    TRACE(1, "%sGlobalCache %p initializing m_globals %p\n",
          isBoxed ? "Boxed" : "",
          this, m_globals);
    ASSERT(m_hint == 0);
  } else {
    TRACE(1, "%sGlobalCache %p cbo %d m_globals %p real globals %p\n",
          isBoxed ? "Boxed" : "",
          this, (int)cacheHandle(), m_globals, getGlobArray());
    ASSERT(m_globals == getGlobArray());
  }

  UNUSED int oldHint = debug ? m_hint : 0;
  TypedValue* retval;
  retval = m_globals->nvGet(name, m_hint, &m_hint);
  if (debug) {
    if (retval != NULL && m_hint == oldHint) {
      Stats::inc(Stats::TgtCache_GlobalHit);
    } else {
      Stats::inc(Stats::TgtCache_GlobalMiss);
    }
  }
  if (!retval) {
    hit = false;

    if (!allowCreate) goto miss;

    VarEnv* ve = g_vmContext->m_varEnvs.front();
    ASSERT(ve->isGlobalScope());
    ASSERT(ve->lookup(name) == NULL);
    TypedValue tv;
    TV_WRITE_NULL(&tv);
    ve->set(name, &tv);
    retval = ve->lookup(name);
  } else {
    hit = true;
  }

  ASSERT(retval);
  if (isBoxed && retval->m_type != KindOfVariant) {
    tvBox(retval);
    ASSERT(retval->m_type == KindOfVariant);
  }
  if (!isBoxed && retval->m_type == KindOfVariant) {
    retval = retval->m_data.ptv;
  }
  ASSERT(!isBoxed || retval->m_type == KindOfVariant);
  ASSERT(!IS_REFCOUNTED_TYPE(retval->m_type) || retval->_count >= 0);

miss:
  // decRef the name if we consumed it.  If we didn't get a global, we
  // need to leave the name for the caller to use before decrefing (to
  // emit warnings).
  if (retval && name->decRefCount() == 0) { name->release(); }
  TRACE(5, "%sGlobalCache::lookup(\"%s\") %p -> (%s) %p t%d\n",
        isBoxed ? "Boxed" : "",
        name->data(),
        retval,
        hit ? "hit" : "miss",
        retval ? retval->m_data.ptv : 0,
        retval ? retval->m_type : 0);
  return retval;
}

TypedValue*
GlobalCache::lookup(Handle handle, StringData* name) {
  GlobalCache* thiz = (GlobalCache*)GlobalCache::cacheAtHandle(handle);
  TypedValue* retval = thiz->lookupImpl<false>(name, false /* allowCreate */);
  ASSERT(!retval || retval->m_type != KindOfVariant);
  return retval;
}

TypedValue*
GlobalCache::lookupCreate(Handle handle, StringData* name) {
  GlobalCache* thiz = (GlobalCache*)GlobalCache::cacheAtHandle(handle);
  TypedValue* retval = thiz->lookupImpl<false>(name, true /* allowCreate */);
  ASSERT(retval->m_type != KindOfVariant);
  return retval;
}

TypedValue*
BoxedGlobalCache::lookup(Handle handle, StringData* name) {
  BoxedGlobalCache* thiz = (BoxedGlobalCache*)
    BoxedGlobalCache::cacheAtHandle(handle);
  TypedValue* retval = thiz->lookupImpl<true>(name, false /* allowCreate */);
  ASSERT(!retval || retval->m_type == KindOfVariant);
  return retval;
}

TypedValue*
BoxedGlobalCache::lookupCreate(Handle handle, StringData* name) {
  BoxedGlobalCache* thiz = (BoxedGlobalCache*)
    BoxedGlobalCache::cacheAtHandle(handle);
  TypedValue* retval = thiz->lookupImpl<true>(name, true /* allowCreate */);
  ASSERT(retval->m_type == KindOfVariant);
  return retval;
}

CacheHandle allocKnownClass(const StringData* name) {
  ASSERT(name != NULL);
  return namedAlloc<NSKnownClass>(name, sizeof(Class*), sizeof(Class*));
}

CacheHandle allocClassInitProp(const StringData* name) {
  return namedAlloc<NSClsInitProp>(name, sizeof(Class::PropInitVec*),
                                   sizeof(Class::PropInitVec*));
}

CacheHandle allocClassInitSProp(const StringData* name) {
  return namedAlloc<NSClsInitSProp>(name, sizeof(HphpArray*),
                                    sizeof(HphpArray*));
}

CacheHandle allocFixedFunction(const StringData* name) {
  return namedAlloc<NSFunction>(name, sizeof(Func*), sizeof(Func*));
}

template<bool checkOnly>
Class*
lookupKnownClass(Class** cache, const StringData* clsName, bool isClass) {
  if (!checkOnly) {
    Stats::inc(Stats::TgtCache_KnownClsHit, -1);
    Stats::inc(Stats::TgtCache_KnownClsMiss, 1);
  }

  Class* cls = *cache;
  ASSERT(!cls); // the caller should already have checked
  VMRegAnchor _;
  EXCEPTION_GATE_ENTER();
  AutoloadHandler::s_instance->invokeHandler(clsName->data());
  cls = *cache;

  if (checkOnly) {
    // If the class still doesn't exist, return flags causing the
    // attribute check in the translated code that called us to fail.
    return (Class*)(uintptr_t)(cls ? cls->attrs() :
      (isClass ? (AttrTrait | AttrInterface) : AttrNone));
  } else if (UNLIKELY(!cls)) {
    undefinedError(Strings::UNKNOWN_CLASS, clsName->data());
  }
  EXCEPTION_GATE_LEAVE();
  return cls;
}
template Class* lookupKnownClass<true>(Class**, const StringData*, bool);
template Class* lookupKnownClass<false>(Class**, const StringData*, bool);

//=============================================================================
// ClassCache

template<>
inline int
ClassCache::hashKey(StringData* sd) {
  return sd->hash();
}

template<>
const Class*
ClassCache::lookup(Handle handle, StringData *name,
                   const void* unused) {
  ClassCache* thiz = cacheAtHandle(handle);
  Pair *pair = thiz->keyToPair(name);
  const StringData* pairSd = pair->m_key;
  if (!stringMatches(pairSd, name)) {
    TRACE(1, "ClassCache miss: %s\n", name->data());
    const NamedEntity *ne = Unit::GetNamedEntity(name);
    Class *c = Unit::lookupClass(ne);
    if (UNLIKELY(!c)) {
      EXCEPTION_GATE_ENTER();
      c = Unit::loadMissingClass(ne, name);
      EXCEPTION_GATE_LEAVE();
      if (UNLIKELY(!c)) {
        undefinedError(Strings::UNKNOWN_CLASS, name->data());
      }
    }
    if (pair->m_key &&
        pair->m_key->decRefCount() == 0) {
      pair->m_key->release();
    }
    pair->m_key = name;
    name->incRefCount();
    pair->m_value = c;
  } else {
    TRACE(1, "ClassCache hit: %s\n", name->data());
  }
  return pair->m_value;
}

//=============================================================================
// PropCache

void* propLookupPrep(CacheHandle& ch, const StringData* name,
                     HomeState hs, CtxState cs, NameState ns) {
  if (false) {
    CacheHandle ch = 0;
    ObjectData* base = NULL;
    StringData* name = NULL;
    TypedValue* stack = 0;
    ActRec* fp = NULL;
    // Make sure the compiler generates code for all of these.
    PropCache::lookup<false>(ch, base, name, stack, fp);
    PropCache::lookup<true>(ch, base, name, stack, fp);
    PropNameCache::lookup<false>(ch, base, name, stack, fp);
    PropNameCache::lookup<true>(ch, base, name, stack, fp);
    PropCtxCache::lookup<false>(ch, base, name, stack, fp);
    PropCtxCache::lookup<true>(ch, base, name, stack, fp);
    PropCtxNameCache::lookup<false>(ch, base, name, stack, fp);
    PropCtxNameCache::lookup<true>(ch, base, name, stack, fp);
  }
  if (cs == STATIC_CONTEXT) {
    if (ns == STATIC_NAME) {
      ch = PropCache::alloc(name);
      if (hs == BASE_CELL) {
        return (void*)PropCache::lookup<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropCache::lookup<true>;
      }
    } else if (ns == DYN_NAME) {
      ch = PropNameCache::alloc();
      if (hs == BASE_CELL) {
        return (void*)PropNameCache::lookup<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropNameCache::lookup<true>;
      }
    }
  } else if (cs == DYN_CONTEXT) {
    if (ns == STATIC_NAME) {
      ch = PropCtxCache::alloc(name);
      if (hs == BASE_CELL) {
        return (void*)PropCtxCache::lookup<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropCtxCache::lookup<true>;
      }
    } else if (ns == DYN_NAME) {
      ch = PropCtxNameCache::alloc();
      if (hs == BASE_CELL) {
        return (void*)PropCtxNameCache::lookup<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropCtxNameCache::lookup<true>;
      }
    }
  }

  NOT_REACHED();
}

void* propSetPrep(CacheHandle& ch, const StringData* name,
                  HomeState hs, CtxState cs, NameState ns) {
  if (false) {
    CacheHandle ch = 0;
    ObjectData* base = NULL;
    StringData* name = NULL;
    int64 val = 0;
    DataType t = KindOfInvalid;
    ActRec* fp = NULL;
    // Make sure the compiler generates code for all of these.
    PropCache::set<false>(ch, base, name, val, t, fp);
    PropCache::set<true>(ch, base, name, val, t, fp);
    PropNameCache::set<false>(ch, base, name, val, t, fp);
    PropNameCache::set<true>(ch, base, name, val, t, fp);
    PropCtxCache::set<false>(ch, base, name, val, t, fp);
    PropCtxCache::set<true>(ch, base, name, val, t, fp);
    PropCtxNameCache::set<false>(ch, base, name, val, t, fp);
    PropCtxNameCache::set<true>(ch, base, name, val, t, fp);
  }
  if (cs == STATIC_CONTEXT) {
    if (ns == STATIC_NAME) {
      ch = PropCache::alloc(name);
      if (hs == BASE_CELL) {
        return (void*)PropCache::set<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropCache::set<true>;
      }
    } else if (ns == DYN_NAME) {
      ch = PropNameCache::alloc();
      if (hs == BASE_CELL) {
        return (void*)PropNameCache::set<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropNameCache::set<true>;
      }
    }
  } else if (cs == DYN_CONTEXT) {
    if (ns == STATIC_NAME) {
      ch = PropCtxCache::alloc(name);
      if (hs == BASE_CELL) {
        return (void*)PropCtxCache::set<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropCtxCache::set<true>;
      }
    } else if (ns == DYN_NAME) {
      ch = PropCtxNameCache::alloc();
      if (hs == BASE_CELL) {
        return (void*)PropCtxNameCache::set<false>;
      } else if (hs == BASE_LOCAL) {
        return (void*)PropCtxNameCache::set<true>;
      }
    }
  }

  NOT_REACHED();
}

template<>
inline int
PropCache::Parent::hashKey(PropKey key) {
  pointer_hash<Class> h;
  return h(key.cls);
}

template<>
inline int
PropNameCache::Parent::hashKey(PropNameKey key) {
  return hash_int64_pair((intptr_t)key.cls, key.name->hash());
}

template<>
inline int
PropCtxCache::Parent::hashKey(PropCtxKey key) {
  return hash_int64_pair((intptr_t)key.cls, (intptr_t)key.ctx);
}

template<>
inline int
PropCtxNameCache::Parent::hashKey(PropCtxNameKey key) {
  return hash_int64_pair(
    hash_int64_pair((intptr_t)key.cls, (intptr_t)key.ctx),
    key.name->hash());
}

template<> inline void
PropCache::incStat() { Stats::inc(Stats::Tx64_PropCache); }

template<> inline void
PropNameCache::incStat() { Stats::inc(Stats::Tx64_PropNameCache); }

template<> inline void
PropCtxCache::incStat() { Stats::inc(Stats::Tx64_PropCtxCache); }

template<> inline void
PropCtxNameCache::incStat() { Stats::inc(Stats::Tx64_PropCtxNameCache); }

template<typename Key, PHPNameSpace ns>
template<bool baseIsLocal>
TypedValue*
PropCacheBase<Key, ns>::lookup(CacheHandle handle, ObjectData* base,
                               StringData* name, TypedValue* stackPtr,
                               ActRec* fp) {
  incStat();
  Class* c = base->getVMClass();
  ASSERT(c != NULL);
  Key key(c, fp, name);
  typename Parent::Self* thiz = Parent::cacheAtHandle(handle);
  typename Parent::Pair* pair = thiz->keyToPair(key);
  if (pair->m_key == key) {
    TypedValue* result = (TypedValue*)((uintptr_t)base + pair->m_value);
    if (UNLIKELY(result->m_type == KindOfUninit)) {
      VMRegAnchor _;
      ASSERT(base->isInstance());
      static_cast<Instance*>(base)->raiseUndefProp(name);
      result = (TypedValue*)&init_null_variant;
    }

    Stats::inc(Stats::TgtCache_PropGetHit);
    return result;
  }
  if (!pair->m_value) {
    Stats::inc(Stats::TgtCache_PropGetFill);
  } else {
    Stats::inc(Stats::TgtCache_PropGetMiss);
  }

  VMRegAnchor _;
  // For getters and extension objects we must provide our own storage
  // for the property; we use the stack cell where the value
  // belongs.
  TypedValue* result = stackPtr;
  // Pseudomains don't always have the same context class
  ASSERT(!curFunc()->isPseudoMain());
  Class* ctx = arGetContextClass(g_vmContext->getFP());
  Instance* instance =
    base->isInstance() ? static_cast<Instance*>(base) : NULL;

  // "Hold" the reference in stackPtr, too.
  TypedValue &ref = *stackPtr;
  tvWriteUninit(&ref);
  EXCEPTION_GATE_ENTER();
  if (LIKELY(instance != NULL)) {
    instance->propW(result, ref, ctx, name);
  } else {
    CStrRef ctxName = ctx ? ctx->nameRef() : null_string;
    tvAsVariant(result) = base->o_get(CStrRef(name), true, ctxName);
  }
  EXCEPTION_GATE_LEAVE();

  ASSERT(result == stackPtr || instance != NULL);
  Slot propIndex;
  if (result == stackPtr) {
    // The property is already in the right place and we already own
    // a reference to it. Clean up and tell our caller there's nothing
    // left to do.
    if (result->m_type == KindOfVariant) {
      tvUnbox(result);
    }
    if (!baseIsLocal && base->decRefCount() == 0) {
      base->release();
    }

    Stats::inc(Stats::TgtCache_PropGetFail);
    return NULL;
  } else if ((propIndex = instance->declPropInd(result)) != kInvalidSlot) {
    // It's a declared property and has a fixed offset from
    // base. Store this in the cache. We need to hold a reference to
    // name in case it's not a static string, and to keep things
    // simple we're relying on these strings getting swept at the end
    // of the request. If we're evicting an existing value we do the
    // decRef now to save memory.
    ASSERT(result != stackPtr);
    name->incRefCount();
    pair->m_key.destroy();
    pair->m_key = key;
    pair->m_value = c->declPropOffset(propIndex);
  } else {
    Stats::inc(Stats::TgtCache_PropGetFail);
  }

  return result;
}

template<typename Key, PHPNameSpace ns>
template<bool baseIsLocal>
TypedValue*
PropCacheBase<Key, ns>::set(CacheHandle ch, ObjectData* base, StringData* name,
                            int64 val, DataType type, ActRec* fp) {
  incStat();
  Class* c = base->getVMClass();
  ASSERT(c != NULL);
  Key key(c, fp, name);
  typename Parent::Self* thiz = Parent::cacheAtHandle(ch);
  typename Parent::Pair* pair = thiz->keyToPair(key);
  if (pair->m_key == key) {
    Stats::inc(Stats::TgtCache_PropSetHit);
    return (TypedValue*)((uintptr_t)base + pair->m_value);
  }
  if (!pair->m_value) {
    Stats::inc(Stats::TgtCache_PropSetFill);
  } else {
    Stats::inc(Stats::TgtCache_PropSetMiss);
  }

  VMRegAnchor _;
  // Pseudomains don't always have the same context class
  ASSERT(!curFunc()->isPseudoMain());
  Class* ctx = arGetContextClass(g_vmContext->getFP());
  Instance* instance =
    base->isInstance() ? static_cast<Instance*>(base) : NULL;
  TypedValue propVal;
  propVal._count = 0;
  propVal.m_type = type;
  propVal.m_data.num = val;
  EXCEPTION_GATE_ENTER();
  if (LIKELY(instance != NULL)) {
    TypedValue* result = instance->setProp(ctx, name, &propVal);
    // setProp will return a real pointer iff it's a declared property
    if (result != NULL) {
      ASSERT(instance->declPropInd(result) != kInvalidSlot);
      name->incRefCount();
      pair->m_key.destroy();
      pair->m_key = key;
      pair->m_value = (uintptr_t)result - (uintptr_t)base;
    } else {
      Stats::inc(Stats::TgtCache_PropSetFail);
    }
  } else {
    base->o_set(CStrRef(name), tvAsCVarRef(&propVal));
    Stats::inc(Stats::TgtCache_PropSetFail);
  }
  EXCEPTION_GATE_LEAVE();

  if (!baseIsLocal && base->decRefCount() == 0) {
    base->release();
  }

  return NULL;
}

/*
 * Constants are raw TypedValues read from TLS storage by emitted code.
 * We must represent the undefined value as KindOfUninit == 0. Constant
 * definition is hooked in the runtime to allocate and update these
 * structures.
 */
CacheHandle allocConstant(StringData* name) {
  BOOST_STATIC_ASSERT(KindOfUninit == 0);
  return namedAlloc<NSConstant>(name, sizeof(TypedValue), sizeof(TypedValue));
}

CacheHandle allocStatic() {
  return ptrToHandle(tl_targetCaches.allocAt(s_frontier, sizeof(TypedValue*),
                                             sizeof(TypedValue*)));
}

void
fillConstant(StringData* name) {
  HandleMutex mtx;
  ASSERT(name);
  Handle ch = allocConstant(name);
  testAndSetBit(allocCnsBit(name));
  TypedValue *val = g_vmContext->getCns(name);
  ASSERT(val);
  *(TypedValue*)handleToPtr(ch) = *val;
}

CacheHandle allocClassConstant(StringData* name) {
  return namedAlloc<NSClassConstant>(name,
                                     sizeof(TypedValue), sizeof(TypedValue));
}

TypedValue*
lookupClassConstant(TypedValue* cache,
                    const NamedEntity* ne,
                    const StringData* cls,
                    const StringData* cns) {
  Stats::inc(Stats::TgtCache_ClsCnsHit, -1);
  Stats::inc(Stats::TgtCache_ClsCnsMiss, 1);

  TypedValue* clsCns;
  EXCEPTION_GATE_ENTER();
  clsCns = g_vmContext->lookupClsCns(ne, cls, cns);
  EXCEPTION_GATE_LEAVE();
  *cache = *clsCns;

  return cache;
}

//=============================================================================
// *SPropCache
//

TypedValue*
SPropCache::lookup(Handle handle, const Class *cls, const StringData *name) {
  // The fast path is in-TC. If we get here, we have already missed.
  VMRegAnchor _;
  SPropCache* thiz = cacheAtHandle(handle);
  Stats::inc(Stats::TgtCache_SPropMiss);
  Stats::inc(Stats::TgtCache_SPropHit, -1);
  ASSERT(cls && name);
  ASSERT(!thiz->m_tv);
  TRACE(3, "SPropCache miss: %s::$%s\n", cls->name()->data(),
        name->data());
  // This is valid only if the lookup comes from an in-class method
  Class *ctx = const_cast<Class*>(cls);
  ASSERT(ctx == arGetContextClass((ActRec*)vmfp()));
  bool visible, accessible;
  TypedValue* val;
  EXCEPTION_GATE_ENTER();
  val = cls->getSProp(ctx, name, visible, accessible);
  EXCEPTION_GATE_LEAVE();
  if (UNLIKELY(!visible)) {
    string methodName;
    string_printf(methodName, "%s::$%s",
                  cls->name()->data(), name->data());
    undefinedError("Invalid static property access: %s", methodName.c_str());
  }
  // We only cache in class references, thus we can always cache them
  // once the property is known to exist
  ASSERT(accessible);
  thiz->m_tv = val;
  TRACE(3, "SPropCache::lookup(\"%s::$%s\") %p -> %p t%d\n",
        cls->name()->data(),
        name->data(),
        val,
        val->m_data.ptv,
        val->m_type);
  ASSERT(val->m_type >= MinDataType && val->m_type < MaxNumDataTypes);
  return val;
}

//=============================================================================
// StaticMethodCache
//

template<typename T, PHPNameSpace ns>
static inline CacheHandle
allocStaticMethodCache(const StringData* clsName,
                       const StringData* methName,
                       const char* ctxName) {
  // Implementation detail of FPushClsMethodD/F: we use "C::M:ctx" as
  // the key for invoking static method "M" on class "C". This
  // composes such a key. "::" is semi-arbitrary, though whatever we
  // choose must delimit possible class and method names, so we might
  // as well ape the source syntax
  const StringData* joinedName =
    StringData::GetStaticString(String(clsName->data()) + String("::") +
                                String(methName->data()) + String(":") +
                                String(ctxName));

  return namedAlloc<ns>(joinedName, sizeof(T), sizeof(T));
}

CacheHandle
StaticMethodCache::alloc(const StringData* clsName,
                         const StringData* methName,
                         const char* ctxName) {
  return allocStaticMethodCache<StaticMethodCache, NSStaticMethod>(
    clsName, methName, ctxName);
}

CacheHandle
StaticMethodFCache::alloc(const StringData* clsName,
                          const StringData* methName,
                          const char* ctxName) {
  return allocStaticMethodCache<StaticMethodFCache, NSStaticMethodF>(
    clsName, methName, ctxName);
}

const Func*
StaticMethodCache::lookup(Handle handle, const NamedEntity *ne,
                          const StringData* clsName,
                          const StringData* methName) {
  StaticMethodCache* thiz = static_cast<StaticMethodCache*>
    (handleToPtr(handle));
  Stats::inc(Stats::TgtCache_StaticMethodMiss);
  Stats::inc(Stats::TgtCache_StaticMethodHit, -1);
  TRACE(1, "miss %s :: %s caller %p\n",
        clsName->data(), methName->data(), __builtin_return_address(0));
  VMRegAnchor _; // needed for lookupClsMethod.

  ActRec* ar = reinterpret_cast<ActRec*>(vmsp() - kNumActRecCells);
  EXCEPTION_GATE_ENTER();
  const Func* f;
  VMExecutionContext* ec = g_vmContext;
  const Class* cls = Unit::loadClass(ne, clsName);
  if (UNLIKELY(!cls)) {
    raise_error(Strings::UNKNOWN_CLASS, clsName->data());
  }
  LookupResult res = ec->lookupClsMethod(f, cls, methName,
                                         NULL, // there may be an active this,
                                               // but we can just fall through
                                               // in that case.
                                         false /*raise*/);
  if (LIKELY(res == MethodFoundNoThis &&
             !f->isAbstract() &&
             f->isStatic())) {
    f->validate();
    TRACE(1, "fill %s :: %s -> %p\n", clsName->data(),
          methName->data(), f);
    // Do the | here instead of on every call.
    thiz->m_cls = (Class*)(uintptr_t(cls) | 1);
    thiz->m_func = f;
    ar->setClass(const_cast<Class*>(cls));
    return f;
  }
  ASSERT(res != MethodFoundWithThis); // Not possible: no this supplied.
  // We've already sync'ed regs; this is some hard case, we might as well
  // just let the interpreter handle this entirely.
  ASSERT(*vmpc() == OpFPushClsMethodD);
  Stats::inc(Stats::Instr_InterpOneFPushClsMethodD);
  Stats::inc(Stats::Instr_TC, -1);
  ec->opFPushClsMethodD();
  // Return whatever func the instruction produced; if nothing was
  // possible we'll either have fataled or thrown.
  ASSERT(ar->m_func);
  ar->m_func->validate();
  // Don't update the cache; this case was too scary to memoize.
  TRACE(1, "unfillable miss %s :: %s -> %p\n", clsName->data(),
        methName->data(), ar->m_func);
  EXCEPTION_GATE_LEAVE();
  // Indicate to the caller that there is no work to do.
  return NULL;
}

const Func*
StaticMethodFCache::lookup(Handle handle, const Class* cls,
                           const StringData* methName) {
  ASSERT(cls);
  StaticMethodFCache* thiz = static_cast<StaticMethodFCache*>
    (handleToPtr(handle));
  Stats::inc(Stats::TgtCache_StaticMethodFMiss);
  Stats::inc(Stats::TgtCache_StaticMethodFHit, -1);
  VMRegAnchor _; // needed for lookupClsMethod.

  EXCEPTION_GATE_ENTER();
  const Func* f;
  VMExecutionContext* ec = g_vmContext;
  LookupResult res = ec->lookupClsMethod(f, cls, methName,
                                         NULL,
                                         false /*raise*/);
  ASSERT(res != MethodFoundWithThis); // Not possible: no this supplied.
  if (LIKELY(res == MethodFoundNoThis && !f->isAbstract())) {
    // We called lookupClsMethod with a NULL this and got back a
    // method that may or may not be static. This implies that
    // lookupClsMethod, given the same class and the same method name,
    // will never return MagicCall*Found or MethodNotFound. It will
    // always return the same f and if we do give it a this it will
    // return MethodFoundWithThis iff (this->instanceof(cls) &&
    // !f->isStatic()). this->instanceof(cls) is always true for
    // FPushClsMethodF because it is only used for self:: and parent::
    // calls. So, if we store f and its staticness we can handle calls
    // with and without this completely in assembly.
    f->validate();
    thiz->m_func = f;
    thiz->m_static = f->isStatic();
    TRACE(1, "fill staticfcache %s :: %s -> %p\n",
          cls->name()->data(), methName->data(), f);
    return f;
  }

  // We've already sync'ed regs; this is some hard case, we might as well
  // just let the interpreter handle this entirely.
  ASSERT(*vmpc() == OpFPushClsMethodF);
  Stats::inc(Stats::Instr_TC, -1);
  Stats::inc(Stats::Instr_InterpOneFPushClsMethodF);
  ec->opFPushClsMethodF();
  EXCEPTION_GATE_LEAVE();

  // We already did all the work so tell our caller to do nothing.
  TRACE(1, "miss staticfcache %s :: %s -> intractable null\n",
        cls->name()->data(), methName->data());
  return NULL;
}

} } } } // HPHP::VM::Transl::TargetCache
