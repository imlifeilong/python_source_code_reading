#ifndef Py_ATOMIC_H
#define Py_ATOMIC_H
#ifdef Py_BUILD_CORE

#include "dynamic_annotations.h"

#include "pyconfig.h"

#if defined(HAVE_STD_ATOMIC)
#include <stdatomic.h>
#endif


#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#endif

/* This is modeled after the atomics interface from C1x, according to
 * the draft at
 * http://www.open-std.org/JTC1/SC22/wg14/www/docs/n1425.pdf.
 * Operations and types are named the same except with a _Py_ prefix
 * and have the same semantics.
 *
 * Beware, the implementations here are deep magic.
 */

#if defined(HAVE_STD_ATOMIC)

typedef enum _Py_memory_order {
    _Py_memory_order_relaxed = memory_order_relaxed,
    _Py_memory_order_acquire = memory_order_acquire,
    _Py_memory_order_release = memory_order_release,
    _Py_memory_order_acq_rel = memory_order_acq_rel,
    _Py_memory_order_seq_cst = memory_order_seq_cst
} _Py_memory_order;

typedef struct _Py_atomic_address {
    atomic_uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    atomic_int _value;
} _Py_atomic_int;

#define _Py_atomic_signal_fence(/*memory_order*/ ORDER) \
    atomic_signal_fence(ORDER)

#define _Py_atomic_thread_fence(/*memory_order*/ ORDER) \
    atomic_thread_fence(ORDER)

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
    atomic_store_explicit(&(ATOMIC_VAL)->_value, NEW_VAL, ORDER)

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
    atomic_load_explicit(&(ATOMIC_VAL)->_value, ORDER)

/* Use builtin atomic operations in GCC >= 4.7 */
#elif defined(HAVE_BUILTIN_ATOMIC)

typedef enum _Py_memory_order {
    _Py_memory_order_relaxed = __ATOMIC_RELAXED,
    _Py_memory_order_acquire = __ATOMIC_ACQUIRE,
    _Py_memory_order_release = __ATOMIC_RELEASE,
    _Py_memory_order_acq_rel = __ATOMIC_ACQ_REL,
    _Py_memory_order_seq_cst = __ATOMIC_SEQ_CST
} _Py_memory_order;

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;

#define _Py_atomic_signal_fence(/*memory_order*/ ORDER) \
    __atomic_signal_fence(ORDER)

#define _Py_atomic_thread_fence(/*memory_order*/ ORDER) \
    __atomic_thread_fence(ORDER)

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
    (assert((ORDER) == __ATOMIC_RELAXED                       \
            || (ORDER) == __ATOMIC_SEQ_CST                    \
            || (ORDER) == __ATOMIC_RELEASE),                  \
     __atomic_store_n(&(ATOMIC_VAL)->_value, NEW_VAL, ORDER))

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER)           \
    (assert((ORDER) == __ATOMIC_RELAXED                       \
            || (ORDER) == __ATOMIC_SEQ_CST                    \
            || (ORDER) == __ATOMIC_ACQUIRE                    \
            || (ORDER) == __ATOMIC_CONSUME),                  \
     __atomic_load_n(&(ATOMIC_VAL)->_value, ORDER))

/* Only support GCC (for expression statements) and x86 (for simple
 * atomic semantics) and MSVC x86/x64/ARM */
#elif defined(__GNUC__) && (defined(__i386__) || defined(__amd64))
typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;


static __inline__ void
_Py_atomic_signal_fence(_Py_memory_order order)
{
    if (order != _Py_memory_order_relaxed)
        __asm__ volatile("":::"memory");
}

static __inline__ void
_Py_atomic_thread_fence(_Py_memory_order order)
{
    if (order != _Py_memory_order_relaxed)
        __asm__ volatile("mfence":::"memory");
}

/* Tell the race checker about this operation's effects. */
static __inline__ void
_Py_ANNOTATE_MEMORY_ORDER(const volatile void *address, _Py_memory_order order)
{
    (void)address;              /* shut up -Wunused-parameter */
    switch(order) {
    case _Py_memory_order_release:
    case _Py_memory_order_acq_rel:
    case _Py_memory_order_seq_cst:
        _Py_ANNOTATE_HAPPENS_BEFORE(address);
        break;
    case _Py_memory_order_relaxed:
    case _Py_memory_order_acquire:
        break;
    }
    switch(order) {
    case _Py_memory_order_acquire:
    case _Py_memory_order_acq_rel:
    case _Py_memory_order_seq_cst:
        _Py_ANNOTATE_HAPPENS_AFTER(address);
        break;
    case _Py_memory_order_relaxed:
    case _Py_memory_order_release:
        break;
    }
}

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
    __extension__ ({ \
        __typeof__(ATOMIC_VAL) atomic_val = ATOMIC_VAL; \
        __typeof__(atomic_val->_value) new_val = NEW_VAL;\
        volatile __typeof__(new_val) *volatile_data = &atomic_val->_value; \
        _Py_memory_order order = ORDER; \
        _Py_ANNOTATE_MEMORY_ORDER(atomic_val, order); \
        \
        /* Perform the operation. */ \
        _Py_ANNOTATE_IGNORE_WRITES_BEGIN(); \
        switch(order) { \
        case _Py_memory_order_release: \
            _Py_atomic_signal_fence(_Py_memory_order_release); \
            /* fallthrough */ \
        case _Py_memory_order_relaxed: \
            *volatile_data = new_val; \
            break; \
        \
        case _Py_memory_order_acquire: \
        case _Py_memory_order_acq_rel: \
        case _Py_memory_order_seq_cst: \
            __asm__ volatile("xchg %0, %1" \
                         : "+r"(new_val) \
                         : "m"(atomic_val->_value) \
                         : "memory"); \
            break; \
        } \
        _Py_ANNOTATE_IGNORE_WRITES_END(); \
    })

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
    __extension__ ({  \
        __typeof__(ATOMIC_VAL) atomic_val = ATOMIC_VAL; \
        __typeof__(atomic_val->_value) result; \
        volatile __typeof__(result) *volatile_data = &atomic_val->_value; \
        _Py_memory_order order = ORDER; \
        _Py_ANNOTATE_MEMORY_ORDER(atomic_val, order); \
        \
        /* Perform the operation. */ \
        _Py_ANNOTATE_IGNORE_READS_BEGIN(); \
        switch(order) { \
        case _Py_memory_order_release: \
        case _Py_memory_order_acq_rel: \
        case _Py_memory_order_seq_cst: \
            /* Loads on x86 are not releases by default, so need a */ \
            /* thread fence. */ \
            _Py_atomic_thread_fence(_Py_memory_order_release); \
            break; \
        default: \
            /* No fence */ \
            break; \
        } \
        result = *volatile_data; \
        switch(order) { \
        case _Py_memory_order_acquire: \
        case _Py_memory_order_acq_rel: \
        case _Py_memory_order_seq_cst: \
            /* Loads on x86 are automatically acquire operations so */ \
            /* can get by with just a compiler fence. */ \
            _Py_atomic_signal_fence(_Py_memory_order_acquire); \
            break; \
        default: \
            /* No fence */ \
            break; \
        } \
        _Py_ANNOTATE_IGNORE_READS_END(); \
        result; \
    })

#elif defined(_MSC_VER)
/*  _Interlocked* functions provide a full memory barrier and are therefore
    enough for acq_rel and seq_cst. If the HLE variants aren't available
    in hardware they will fall back to a full memory barrier as well.

    This might affect performance but likely only in some very specific and
    hard to meassure scenario.
*/
#if defined(_M_IX86) || defined(_M_X64)
typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

typedef struct _Py_atomic_address {
    volatile uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    volatile int _value;
} _Py_atomic_int;


#if defined(_M_X64)
#define _Py_atomic_store_64bit(ATOMIC_VAL, NEW_VAL, ORDER) \
    switch (ORDER) { \
    case _Py_memory_order_acquire: \
      _InterlockedExchange64_HLEAcquire((__int64 volatile*)ATOMIC_VAL, (__int64)NEW_VAL); \
      break; \
    case _Py_memory_order_release: \
      _InterlockedExchange64_HLERelease((__int64 volatile*)ATOMIC_VAL, (__int64)NEW_VAL); \
      break; \
    default: \
      _InterlockedExchange64((__int64 volatile*)ATOMIC_VAL, (__int64)NEW_VAL); \
      break; \
  }
#else
#define _Py_atomic_store_64bit(ATOMIC_VAL, NEW_VAL, ORDER) ((void)0);
#endif

#define _Py_atomic_store_32bit(ATOMIC_VAL, NEW_VAL, ORDER) \
  switch (ORDER) { \
  case _Py_memory_order_acquire: \
    _InterlockedExchange_HLEAcquire((volatile long*)ATOMIC_VAL, (int)NEW_VAL); \
    break; \
  case _Py_memory_order_release: \
    _InterlockedExchange_HLERelease((volatile long*)ATOMIC_VAL, (int)NEW_VAL); \
    break; \
  default: \
    _InterlockedExchange((volatile long*)ATOMIC_VAL, (int)NEW_VAL); \
    break; \
  }

#if defined(_M_X64)
/*  This has to be an intptr_t for now.
    gil_created() uses -1 as a sentinel value, if this returns
    a uintptr_t it will do an unsigned compare and crash
*/
inline intptr_t _Py_atomic_load_64bit(volatile uintptr_t* value, int order) {
    __int64 old;
    switch (order) {
    case _Py_memory_order_acquire:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64_HLEAcquire((volatile __int64*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_release:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64_HLERelease((volatile __int64*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_relaxed:
      old = *value;
      break;
    default:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64((volatile __int64*)value, old, old) != old);
      break;
    }
    }
    return old;
}

#else
#define _Py_atomic_load_64bit(ATOMIC_VAL, ORDER) *ATOMIC_VAL
#endif

inline int _Py_atomic_load_32bit(volatile int* value, int order) {
    long old;
    switch (order) {
    case _Py_memory_order_acquire:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange_HLEAcquire((volatile long*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_release:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange_HLERelease((volatile long*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_relaxed:
      old = *value;
      break;
    default:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange((volatile long*)value, old, old) != old);
      break;
    }
    }
    return old;
}
// 根据变量的大小（32 位或 64 位）选择合适的函数来执行原子存储操作，
// 并支持指定的内存顺序。这样可以确保在多线程环境中对共享变量的存储操作是线程安全的，
// 并且满足所需的内存模型约束。
#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
  if (sizeof(*ATOMIC_VAL._value) == 8) { \
    _Py_atomic_store_64bit((volatile long long*)ATOMIC_VAL._value, NEW_VAL, ORDER) } else { \
    _Py_atomic_store_32bit((volatile long*)ATOMIC_VAL._value, NEW_VAL, ORDER) }

// 用于以指定的内存顺序从原子变量 ATOMIC_VAL 中读取值。这是为了在多线程环境中保证数据读取的原子性和线程安全性。
// 通过 sizeof(*(ATOMIC_VAL._value)) 检查 ATOMIC_VAL 所指向的值的大小，以确定它是 8 字节（64 位）还是 4 字节（32 位）
// _Py_atomic_load_64bit：如果 ATOMIC_VAL 是 64 位的，则调用此函数读取值。
// _Py_atomic_load_32bit：如果 ATOMIC_VAL 是 32 位的，则调用此函数读取值。
// 这两个函数都需要传入一个指向值的指针，并指定内存顺序（ORDER），确保读取操作是线程安全的。
// ORDER 参数指定了内存顺序（Memory Order），例如可能是 _Py_memory_order_acquire 或 _Py_memory_order_relaxed 等，这个参数控制了在读取时的内存屏障行为。
#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
  ( \
    sizeof(*(ATOMIC_VAL._value)) == 8 ? \
    _Py_atomic_load_64bit((volatile long long*)ATOMIC_VAL._value, ORDER) : \
    _Py_atomic_load_32bit((volatile long*)ATOMIC_VAL._value, ORDER) \
  )
#elif defined(_M_ARM) || defined(_M_ARM64)
typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

typedef struct _Py_atomic_address {
    volatile uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    volatile int _value;
} _Py_atomic_int;


#if defined(_M_ARM64)
#define _Py_atomic_store_64bit(ATOMIC_VAL, NEW_VAL, ORDER) \
    switch (ORDER) { \
    case _Py_memory_order_acquire: \
      _InterlockedExchange64_acq((__int64 volatile*)ATOMIC_VAL, (__int64)NEW_VAL); \
      break; \
    case _Py_memory_order_release: \
      _InterlockedExchange64_rel((__int64 volatile*)ATOMIC_VAL, (__int64)NEW_VAL); \
      break; \
    default: \
      _InterlockedExchange64((__int64 volatile*)ATOMIC_VAL, (__int64)NEW_VAL); \
      break; \
  }
#else
#define _Py_atomic_store_64bit(ATOMIC_VAL, NEW_VAL, ORDER) ((void)0);
#endif

#define _Py_atomic_store_32bit(ATOMIC_VAL, NEW_VAL, ORDER) \
  switch (ORDER) { \
  case _Py_memory_order_acquire: \
    _InterlockedExchange_acq((volatile long*)ATOMIC_VAL, (int)NEW_VAL); \
    break; \
  case _Py_memory_order_release: \
    _InterlockedExchange_rel((volatile long*)ATOMIC_VAL, (int)NEW_VAL); \
    break; \
  default: \
    _InterlockedExchange((volatile long*)ATOMIC_VAL, (int)NEW_VAL); \
    break; \
  }

#if defined(_M_ARM64)
/*  This has to be an intptr_t for now.
    gil_created() uses -1 as a sentinel value, if this returns
    a uintptr_t it will do an unsigned compare and crash
*/
inline intptr_t _Py_atomic_load_64bit(volatile uintptr_t* value, int order) {
    uintptr_t old;
    switch (order) {
    case _Py_memory_order_acquire:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64_acq(value, old, old) != old);
      break;
    }
    case _Py_memory_order_release:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64_rel(value, old, old) != old);
      break;
    }
    case _Py_memory_order_relaxed:
      old = *value;
      break;
    default:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64(value, old, old) != old);
      break;
    }
    }
    return old;
}

#else
#define _Py_atomic_load_64bit(ATOMIC_VAL, ORDER) *ATOMIC_VAL
#endif

inline int _Py_atomic_load_32bit(volatile int* value, int order) {
    int old;
    switch (order) {
    case _Py_memory_order_acquire:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange_acq(value, old, old) != old);
      break;
    }
    case _Py_memory_order_release:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange_rel(value, old, old) != old);
      break;
    }
    case _Py_memory_order_relaxed:
      old = *value;
      break;
    default:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange(value, old, old) != old);
      break;
    }
    }
    return old;
}

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
  if (sizeof(*ATOMIC_VAL._value) == 8) { \
    _Py_atomic_store_64bit(ATOMIC_VAL._value, NEW_VAL, ORDER) } else { \
    _Py_atomic_store_32bit(ATOMIC_VAL._value, NEW_VAL, ORDER) }

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
  ( \
    sizeof(*(ATOMIC_VAL._value)) == 8 ? \
    _Py_atomic_load_64bit(ATOMIC_VAL._value, ORDER) : \
    _Py_atomic_load_32bit(ATOMIC_VAL._value, ORDER) \
  )
#endif
#else  /* !gcc x86  !_msc_ver */
typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;
/* Fall back to other compilers and processors by assuming that simple
   volatile accesses are atomic.  This is false, so people should port
   this. */
#define _Py_atomic_signal_fence(/*memory_order*/ ORDER) ((void)0)
#define _Py_atomic_thread_fence(/*memory_order*/ ORDER) ((void)0)
#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
    ((ATOMIC_VAL)->_value = NEW_VAL)
#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
    ((ATOMIC_VAL)->_value)
#endif

/* Standardized shortcuts. */
#define _Py_atomic_store(ATOMIC_VAL, NEW_VAL) \
    _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, _Py_memory_order_seq_cst)
#define _Py_atomic_load(ATOMIC_VAL) \
    _Py_atomic_load_explicit(ATOMIC_VAL, _Py_memory_order_seq_cst)

/* Python-local extensions */
// 内存顺序将 NEW_VAL 存储到原子变量 ATOMIC_VAL 中。这种存储操作是线程安全的，但不会强制执行任何特定的内存顺序约束
// _Py_memory_order_relaxed 是一种内存顺序选项，它不会引入任何内存屏障。使用这种顺序意味着操作仅保证存储的原子性，
// 而不保证存储操作在多个线程中的执行顺序或可见性。这适用于对性能要求较高且不依赖顺序性的场景
#define _Py_atomic_store_relaxed(ATOMIC_VAL, NEW_VAL) \
    _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, _Py_memory_order_relaxed)
// 内存顺序从原子变量 ATOMIC_VAL 中加载（读取）值。此加载操作是线程安全的，同样不会强制执行任何特定的内存顺序约束
// _Py_memory_order_relaxed 选项仅保证读取的原子性，不会引入内存屏障，也不保证多个线程间的操作顺序或可见性
#define _Py_atomic_load_relaxed(ATOMIC_VAL) \
    _Py_atomic_load_explicit(ATOMIC_VAL, _Py_memory_order_relaxed)
#endif  /* Py_BUILD_CORE */
#endif  /* Py_ATOMIC_H */
