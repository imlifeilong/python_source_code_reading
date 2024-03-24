#ifndef Py_LIMITED_API
#ifndef Py_LONGINTREPR_H
#define Py_LONGINTREPR_H
#ifdef __cplusplus
extern "C" {
#endif


/* This is published for the benefit of "friends" marshal.c and _decimal.c. */

/* Parameters of the integer representation.  There are two different
   sets of parameters: one set for 30-bit digits, stored in an unsigned 32-bit
   integer type, and one set for 15-bit digits with each digit stored in an
   unsigned short.  The value of PYLONG_BITS_IN_DIGIT, defined either at
   configure time or in pyport.h, is used to decide which digit size to use.

   Type 'digit' should be able to hold 2*PyLong_BASE-1, and type 'twodigits'
   should be an unsigned integer type able to hold all integers up to
   PyLong_BASE*PyLong_BASE-1.  x_sub assumes that 'digit' is an unsigned type,
   and that overflow is handled by taking the result modulo 2**N for some N >
   PyLong_SHIFT.  The majority of the code doesn't care about the precise
   value of PyLong_SHIFT, but there are some notable exceptions:

   - long_pow() requires that PyLong_SHIFT be divisible by 5

   - PyLong_{As,From}ByteArray require that PyLong_SHIFT be at least 8

   - long_hash() requires that PyLong_SHIFT is *strictly* less than the number
     of bits in an unsigned long, as do the PyLong <-> long (or unsigned long)
     conversion functions

   - the Python int <-> size_t/Py_ssize_t conversion functions expect that
     PyLong_SHIFT is strictly less than the number of bits in a size_t

   - the marshal code currently expects that PyLong_SHIFT is a multiple of 15

   - NSMALLNEGINTS and NSMALLPOSINTS should be small enough to fit in a single
     digit; with the current values this forces PyLong_SHIFT >= 9

  The values 15 and 30 should fit all of the above requirements, on any
  platform.
*/
// PYLONG_BITS_IN_DIGIT 值为30表示64位系统，值为15表示32位系统
#if PYLONG_BITS_IN_DIGIT == 30
// uint32_t 是一个无符号32位整数类型，通常定义在 <stdint.h>（C）或 <cstdint>（C++）头文件中
typedef uint32_t digit; // 无符号的整数
typedef int32_t sdigit; /* 有符号的整数 signed variant of digit */
typedef uint64_t twodigits;
typedef int64_t stwodigits; /* signed variant of twodigits */
#define PyLong_SHIFT    30
#define _PyLong_DECIMAL_SHIFT   9 /* max(e such that 10**e fits in a digit) */
#define _PyLong_DECIMAL_BASE    ((digit)1000000000) /* 10 ** DECIMAL_SHIFT */
#elif PYLONG_BITS_IN_DIGIT == 15
// unsigned short 一个16位的无符号整数类型，这意味着它可以表示从0到65535（即2^16 - 1）的整数值
typedef unsigned short digit; 
typedef short sdigit; /* signed variant of digit */
typedef unsigned long twodigits;
typedef long stwodigits; /* signed variant of twodigits */
#define PyLong_SHIFT    15
#define _PyLong_DECIMAL_SHIFT   4 /* max(e such that 10**e fits in a digit) */
#define _PyLong_DECIMAL_BASE    ((digit)10000) /* 10 ** DECIMAL_SHIFT */
#else
#error "PYLONG_BITS_IN_DIGIT should be 15 or 30"
#endif
/* 1 << PyLong_SHIFT：这个表达式将数字 1 左移 PyLong_SHIFT 位。左移操作实际上就是将数字乘以 2 的 PyLong_SHIFT 次方
   如果 PyLong_SHIFT 是 30，那么 1 << 30 就会得到 2^30，即 1073741824。
*/
#define PyLong_BASE     ((digit)1 << PyLong_SHIFT)
#define PyLong_MASK     ((digit)(PyLong_BASE - 1)) // 这是一个掩码，通常定义为 PyLong_BASE - 1

#if PyLong_SHIFT % 5 != 0
#error "longobject.c requires that PyLong_SHIFT be divisible by 5"
#endif

/* Long integer representation.
   The absolute value of a number is equal to
        SUM(for i=0 through abs(ob_size)-1) ob_digit[i] * 2**(SHIFT*i)
   Negative numbers are represented with ob_size < 0;
   zero is represented by ob_size == 0.
   In a normalized number, ob_digit[abs(ob_size)-1] (the most significant
   digit) is never zero.  Also, in all cases, for all valid i,
        0 <= ob_digit[i] <= MASK.
   The allocation function takes care of allocating extra memory
   so that ob_digit[0] ... ob_digit[abs(ob_size)-1] are actually available.

   CAUTION:  Generic code manipulating subtypes of PyVarObject has to
   aware that ints abuse  ob_size's sign bit.
   长整数表示。
   一个数的绝对值等于SUM(for i=0 through abs(ob_size)-1) ob_digit[i] * 2**(SHIFT*i)
   负数用ob_size < 0表示;
   0由ob_size == 0表示。
   在规范化数字中，ob_digit[abs(ob_size)-1](最高有效数字)永远不会为零。
   同样，在所有情况下，对于所有有效的i， 0 <= ob_digit[i] <= MASK.
   分配函数负责分配额外的内存，以便ob_digit[0]…ob_digit[abs(ob_size)-1]实际可用。
   注意:操作PyVarObject子类型的泛型代码必须意识到int滥用ob_size的符号位。
*/

struct _longobject {
    PyObject_VAR_HEAD
    // 定义了一个数组 ob_digit，其类型为 digit（即uint32_t），该数组只有一个元素
    digit ob_digit[1];
};

PyAPI_FUNC(PyLongObject *) _PyLong_New(Py_ssize_t);

/* Return a copy of src. */
PyAPI_FUNC(PyObject *) _PyLong_Copy(PyLongObject *src);

#ifdef __cplusplus
}
#endif
#endif /* !Py_LONGINTREPR_H */
#endif /* Py_LIMITED_API */
