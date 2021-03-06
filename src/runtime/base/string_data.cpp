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

#include <runtime/base/string_data.h>
#include <runtime/base/shared/shared_variant.h>
#include <runtime/base/zend/zend_functions.h>
#include <runtime/base/util/exceptions.h>
#include <util/alloc.h>
#include <math.h>
#include <runtime/base/zend/zend_string.h>
#include <runtime/base/zend/zend_strtod.h>
#include <runtime/base/complex_types.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/runtime_error.h>
#include <runtime/base/type_conversions.h>
#include <runtime/base/builtin_functions.h>
#include <tbb/concurrent_hash_map.h>

namespace HPHP {

IMPLEMENT_SMART_ALLOCATION_HOT(StringData, SmartAllocatorImpl::NeedSweep);
///////////////////////////////////////////////////////////////////////////////
// constructor and destructor

// The (void *) value is not used.
typedef tbb::concurrent_hash_map<const StringData *, void *,
                                 StringDataHashCompare> StringDataMap;
static StringDataMap *s_stringDataMap;

StringData *StringData::GetStaticString(const StringData *str) {
  StringDataMap::const_accessor acc;
  if (!s_stringDataMap) s_stringDataMap = new StringDataMap();
  if (s_stringDataMap->find(acc, str)) {
    return const_cast<StringData*>(acc->first);
  }
  // Lookup failed, so do the hard work of creating a StringData with its own
  // copy of the key string, so that the atomic insert() has a permanent key.
  StringData *sd = new StringData(str->data(), str->size(), CopyString);
  sd->setStatic();
  if (!s_stringDataMap->insert(acc, sd)) {
    delete sd;
  }
  ASSERT(acc->first != NULL);
  return const_cast<StringData*>(acc->first);
}

StringData *StringData::GetStaticString(const std::string &str) {
  StringData sd(str.c_str(), str.size(), AttachLiteral);
  return GetStaticString(&sd);
}

StringData *StringData::GetStaticString(const char *str) {
  StringData sd(str, strlen(str), AttachLiteral);
  return GetStaticString(&sd);
}

void StringData::initLiteral(const char* data) {
  return initLiteral(data, strlen(data));
}

void StringData::initLiteral(const char* data, int len) {
  ASSERT(data && len >= 0 && data[len] == '\0'); // check well formed string.
  if (uint32_t(len) > MaxSize) {
    throw InvalidArgumentException("len>=2^30", len);
  }
  _count = 0;
  m_data = data;
  m_len = len | IsLiteral;
  m_hash = 0;
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, m_data);
}

HOT_FUNC
void StringData::initAttach(const char* data) {
  return initAttach(data, strlen(data));
}

HOT_FUNC
void StringData::initAttach(const char* data, int len) {
  ASSERT(data && len >= 0 && data[len] == '\0'); // check well formed string.
  if (uint32_t(len) > MaxSize) {
    throw InvalidArgumentException("len>=2^30", len);
  }
  _count = 0;
  m_hash = 0;
  if (len) {
    m_len = len;
    m_data = data;
  } else {
    free((void*)data); // we don't really need a malloc-ed empty string
    m_len = len | IsLiteral;
    m_data = "";
  }
  ASSERT(m_data);
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, m_data);
}

HOT_FUNC
void StringData::initCopy(const char* data) {
  return initCopy(data, strlen(data));
}

HOT_FUNC
void StringData::initCopy(const char* data, int len) {
  ASSERT(data && len >= 0); // check well formed string slice
  if (uint32_t(len) > MaxSize) {
    throw InvalidArgumentException("len>=2^30", len);
  }
  _count = 0;
  m_hash = 0;
  if (len) {
    char *buf = (char*)malloc(len + 1);
    memcpy(buf, data, len);
    buf[len] = '\0';
    m_len = len;
    m_data = buf;
  } else {
    m_len = len | IsLiteral;
    m_data = "";
  }
  ASSERT(m_data);
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, m_data);
}

HOT_FUNC
StringData::StringData(SharedVariant *shared)
  : _count(0), m_len(0) {
  ASSERT(shared);
  shared->incRef();
  m_data = shared->stringData();
  m_len = shared->stringLength() | IsShared;
  m_shared = shared;
  ASSERT(m_data);
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, m_data);
}

HOT_FUNC
void StringData::releaseData() {
  if ((m_len & IsLiteral) == 0) {
    if (isShared()) {
      m_shared->decRef();
      m_shared = NULL;
    } else if (m_data) {
      free((void*)m_data);
      m_data = NULL;
    }
  }
}

void StringData::attach(const char *data, int len) {
  ASSERT(data && len >= 0 && data[len] == '\0'); // well formed?
  if (uint32_t(len) > MaxSize) {
    throw InvalidArgumentException("len>=2^30", len);
  }
  releaseData();
  if (len) {
    m_len = len;
    m_data = data;
  } else {
    free((void*)data); // we don't really need a malloc-ed empty string
    m_len = len | IsLiteral;
    m_data = "";
  }
  m_hash = 0;
  ASSERT(m_data);
}

void StringData::append(const char *s, int len) {
  if (len == 0) return;

  if (len < 0 || (len & IsMask)) {
    throw InvalidArgumentException("len>=2^30", len);
  }

  ASSERT(!isStatic()); // never mess around with static strings!

  if (!isMalloced()) {
    int newlen;
    // We are mutating, so we don't need to repropagate our own taint
    m_data = string_concat(m_data, size(), s, len, newlen);
    if (isShared()) {
      m_shared->decRef();
    }
    m_len = newlen;
    m_hash = 0;
  } else if (m_data == s) {
    int newlen;
    // We are mutating, so we don't need to repropagate our own taint
    char *newdata = string_concat(m_data, size(), s, len, newlen);
    releaseData();
    m_hash = 0;
    m_data = newdata;
    m_len = newlen;
  } else {
    int dataLen = size();
    ASSERT((m_data > s && m_data - s > len) ||
           (m_data < s && s - m_data > dataLen)); // no overlapping
    m_len = len + dataLen;
    m_data = (const char*)realloc((void*)m_data, m_len + 1);
    // The memcpy here will also copy the NULL-terminator for us
    memcpy((void*)(m_data + dataLen), s, len+1);
    m_hash = 0;
  }

  if (m_len & IsMask) {
    int len = m_len;
    m_len &= ~IsMask;
    releaseData();
    m_hash = 0;
    m_data = NULL;
    m_len = 0;
    throw FatalErrorException(0, "String length exceeded 2^30 - 1: %d", len);
  }

  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, m_data);
}

StringData *StringData::copy(bool sharedMemory /* = false */) const {
  if (isStatic()) {
    // Static strings cannot change, and are always available.
    return const_cast<StringData *>(this);
  }
  if (sharedMemory) {
    // Even if it's literal, it might come from hphpi's class info
    // which will be freed at the end of the request, and so must be
    // copied.
    return new StringData(data(), size(), CopyString);
  } else {
    if (isLiteral()) {
      return NEW(StringData)(data(), size(), AttachLiteral);
    }
    return NEW(StringData)(data(), size(), CopyString);
  }
}

void StringData::escalate() {
  ASSERT(isImmutable() && !isStatic());

  int len = size();
  ASSERT(len);

  char *buf = (char*)malloc(len+1);
  memcpy(buf, m_data, len);
  buf[len] = '\0';
  m_len = len;
  m_data = buf;
  // clear precomputed hashcode
  m_hash = 0;
}

StringData *StringData::Escalate(StringData *in) {
  if (!in) return NEW(StringData)();
  if (in->_count != 1 || in->isImmutable()) {
    StringData *ret = NEW(StringData)(in->data(), in->size(), CopyString);
    return ret;
  }
  in->m_hash = 0;
  return in;
}

void StringData::dump() const {
  const char *p = m_data;
  int len = size();

  printf("StringData(%d) (%s%s%s%d): [", _count,
         isLiteral() ? "literal " : "",
         isShared() ? "shared " : "",
         isStatic() ? "static " : "",
         len);
  for (int i = 0; i < len; i++) {
    char ch = p[i];
    if (isprint(ch)) {
      std::cout << ch;
    } else {
      printf("\\x%02x", ch);
    }
  }
#ifdef TAINTED
  printf("\n");
  this->getTaintDataRefConst().dump();
#endif
  printf("]\n");
}

static StringData** precompute_chars() ATTRIBUTE_COLD;
static StringData** precompute_chars() {
  StringData** raw = new StringData*[256];
  for (int i = 0; i < 256; i++) {
    char s[2] = { (char)i, 0 };
    StringData str(s, 1, CopyString);
    raw[i] = StringData::GetStaticString(&str);
  }
  return raw;
}

static StringData** precomputed_chars = precompute_chars();

HOT_FUNC
StringData* StringData::GetStaticString(char c) {
  return precomputed_chars[(uint8_t)c];
}

HOT_FUNC
StringData *StringData::getChar(int offset) const {
  if (offset >= 0 && offset < size()) {
    return GetStaticString(m_data[offset]);
  }

  raise_notice("Uninitialized string offset: %d", offset);
  return NEW(StringData)("", 0, AttachLiteral);
}

// mutations
void StringData::setChar(int offset, CStrRef substring) {
  ASSERT(!isStatic());
  if (offset >= 0) {
    int len = size();
    if (len == 0) {
      // PHP will treat data as an array and we don't want to follow that.
      throw OffsetOutOfRangeException();
    }

    if (offset < len) {
      setChar(offset, substring.empty() ? '\0' : substring.data()[0]);
    } else if (offset > RuntimeOption::StringOffsetLimit) {
      throw OffsetOutOfRangeException();
    } else {
      int newlen = offset + 1;
      char *buf = (char *)Util::safe_malloc(newlen + 1);
      memset(buf, ' ', newlen);
      buf[newlen] = 0;
    // We are mutating, so we don't need to repropagate our own taint
      memcpy(buf, m_data, len);
      if (!substring.empty()) buf[offset] = substring.data()[0];
      attach(buf, newlen);
    }
  }
}

void StringData::setChar(int offset, char ch) {
  ASSERT(offset >= 0 && offset < size());
  ASSERT(!isStatic());
  if (isImmutable()) {
    escalate();
  }
  ((char*)m_data)[offset] = ch;
  m_hash = 0; // clear hash since we modified the string.
}

void StringData::inc() {
  ASSERT(!isStatic());
  ASSERT(!empty());
  if (isImmutable()) {
    escalate();
  }
  int len = size();
  char *overflowed = increment_string((char *)m_data, len);
  if (overflowed) {
    attach(overflowed, len);
  } else {
    // We didn't overflow but we did modify the string.
    m_hash = 0;
  }
}

void StringData::negate() {
  if (empty()) return;
  // Assume we're a fresh mutable copy.
  ASSERT(!isImmutable() && _count <= 1 && m_hash == 0);
  char *buf = (char*)m_data;
  int len = size();
  for (int i = 0; i < len; i++) {
    buf[i] = ~(buf[i]);
  }
}

void StringData::set(CStrRef key, CStrRef v) {
  setChar(key.toInt32(), v);
}

void StringData::set(CVarRef key, CStrRef v) {
  setChar(key.toInt32(), v);
}

void StringData::preCompute() const {
  ASSERT(!isShared()); // because we are gonna reuse the space!
  // We don't want to collect taint for a hash
  m_hash = hash_string(m_data, size());
  ASSERT(m_hash >= 0);
  int64 lval; double dval;
  if (isNumericWithVal(lval, dval, 1) == KindOfNull) {
    m_hash |= (1ull << 63);
  }
}

void StringData::setStatic() const {
  _count = RefCountStaticValue;
  preCompute();
}

///////////////////////////////////////////////////////////////////////////////
// type conversions

DataType StringData::isNumericWithVal(int64 &lval, double &dval,
                                      int allow_errors) const {
  if (m_hash < 0) return KindOfNull;
  DataType ret = KindOfNull;
  int len = size();
  if (len) {
    // Not involved in further string construction/mutation; no taint pickup
    ret = is_numeric_string(m_data, size(), &lval, &dval, allow_errors);
    if (ret == KindOfNull && !isShared() && allow_errors) {
      m_hash |= (1ull << 63);
    }
  }
  return ret;
}

bool StringData::isNumeric() const {
  if (isStatic()) return (m_hash >= 0);
  int64 lval; double dval;
  DataType ret = isNumericWithVal(lval, dval, 0);
  switch (ret) {
  case KindOfNull:   return false;
  case KindOfInt64:
  case KindOfDouble: return true;
  default:
    ASSERT(false);
    break;
  }
  return false;
}

bool StringData::isInteger() const {
  if (m_hash < 0) return false;
  int64 lval; double dval;
  DataType ret = isNumericWithVal(lval, dval, 0);
  switch (ret) {
  case KindOfNull:   return false;
  case KindOfInt64:  return true;
  case KindOfDouble: return false;
  default:
    ASSERT(false);
    break;
  }
  return false;
}

bool StringData::isValidVariableName() const {
  // Not involved in further string construction/mutation; no taint pickup
  return is_valid_var_name(m_data, size());
}

int64 StringData::hashForIntSwitch(int64 firstNonZero, int64 noMatch) const {
  int64 lval; double dval;
  DataType ret = isNumericWithVal(lval, dval, 1);
  switch (ret) {
  case KindOfNull:
    // if the string is not a number, it matches 0
    return 0;
  case KindOfInt64:
    return lval;
  case KindOfDouble:
    return Variant::DoubleHashForIntSwitch(dval, noMatch);
  default:
    break;
  }
  ASSERT(false);
  return 0;
}

int64 StringData::hashForStringSwitch(
    int64 firstTrueCaseHash,
    int64 firstNullCaseHash,
    int64 firstFalseCaseHash,
    int64 firstZeroCaseHash,
    int64 firstHash,
    int64 noMatchHash,
    bool &needsOrder) const {
  int64 lval; double dval;
  DataType ret = isNumericWithVal(lval, dval, 1);
  needsOrder = false;
  switch (ret) {
  case KindOfNull:
    return empty() ? firstNullCaseHash : hash();
  case KindOfInt64:
    return lval;
  case KindOfDouble:
    return (int64) dval;
  default:
    break;
  }
  ASSERT(false);
  return 0;
}

bool StringData::toBoolean() const {
  return !empty() && !isZero();
}

int64 StringData::toInt64(int base /* = 10 */) const {
  // Taint absorbtion unnecessary; taint is recreated later for numerics
  return strtoll(m_data, NULL, base);
}

double StringData::toDouble() const {
  int len = size();
  // Taint absorbtion unnecessary; taint is recreated later for numerics
  if (len) return zend_strtod(m_data, NULL);
  return 0;
}

DataType StringData::toNumeric(int64 &lval, double &dval) const {
  if (m_hash < 0) return KindOfString;
  DataType ret = isNumericWithVal(lval, dval, 0);
  if (ret == KindOfInt64 || ret == KindOfDouble) return ret;
  return KindOfString;
}

///////////////////////////////////////////////////////////////////////////////
// comparisons

HOT_FUNC
int StringData::numericCompare(const StringData *v2) const {
  ASSERT(v2);

  int64 lval1, lval2;
  double dval1, dval2;
  DataType ret1, ret2;
  if ((ret1 = isNumericWithVal(lval1, dval1, 0)) == KindOfNull ||
      (ret1 == KindOfDouble && !finite(dval1)) ||
      (ret2 = v2->isNumericWithVal(lval2, dval2, 0)) == KindOfNull ||
      (ret2 == KindOfDouble && !finite(dval2))) {
    return -2;
  }
  if (ret1 == KindOfInt64 && ret2 == KindOfInt64) {
    if (lval1 > lval2) return 1;
    if (lval1 == lval2) return 0;
    return -1;
  }
  if (ret1 == KindOfDouble && ret2 == KindOfDouble) {
    if (dval1 > dval2) return 1;
    if (dval1 == dval2) return 0;
    return -1;
  }
  if (ret1 == KindOfDouble) {
    ASSERT(ret2 == KindOfInt64);
    dval2 = (double)lval2;
  } else {
    ASSERT(ret1 == KindOfInt64);
    ASSERT(ret2 == KindOfDouble);
    dval1 = (double)lval1;
  }

  if (dval1 > dval2) return 1;
  if (dval1 == dval2) return 0;
  return -1;
}

HOT_FUNC
int StringData::compare(const StringData *v2) const {
  ASSERT(v2);

  if (v2 == this) return 0;

  int ret = numericCompare(v2);
  if (ret < -1) {
    int len1 = size();
    int len2 = v2->size();
    int len = len1 < len2 ? len1 : len2;
    // No taint absorption on self-contained string ops like compare
    ret = memcmp(m_data, v2->m_data, len);
    if (ret) return ret;
    if (len1 == len2) return 0;
    return len < len1 ? 1 : -1;
  }
  return ret;
}

HOT_FUNC
int64 StringData::getSharedStringHash() const {
  ASSERT(isShared());
  return m_shared->stringHash();
}

HOT_FUNC
int64 StringData::hashHelper() const {
  // We don't want to collect taint for a hash
  int64 h = hash_string_inline(m_data, size());
  m_hash |= h;
  return h;
}

///////////////////////////////////////////////////////////////////////////////
// Debug

std::string StringData::toCPPString() const {
  return std::string(m_data, size());
}

///////////////////////////////////////////////////////////////////////////////
}
