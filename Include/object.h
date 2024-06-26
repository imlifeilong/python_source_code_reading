#ifndef Py_OBJECT_H
#define Py_OBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


/* Object and type object interface 对象和类型对象接口*/

/*
Objects are structures allocated on the heap.  Special rules apply to
the use of objects to ensure they are properly garbage-collected.
Objects are never allocated statically or on the stack; they must be
accessed through special macros and functions only.  (Type objects are
exceptions to the first rule; the standard types are represented by
statically initialized type objects, although work on type/class unification
for Python 2.2 made it possible to have heap-allocated type objects too).
对象是分配在堆上的结构。特殊规则适用于对象的使用，以确保它们被正确地垃圾收集。
对象永远不会静态分配或在栈上分配； 它们只能通过特殊的宏和函数来访问。 （类型对象是第一条规则的例外；标准类型由静态初始化的类型对象，
尽管 Python 2.2 的类型/类统一工作也使得拥有堆分配的类型对象成为可能）。

An object has a 'reference count' that is increased or decreased when a
pointer to the object is copied or deleted; when the reference count
reaches zero there are no references to the object left and it can be
removed from the heap.
对象有一个“引用计数”，当复制或删除指向该对象的指针时，该引用计数会增加或减少；
当引用计数达到零时，不再有对该对象的引用，可以将其从堆中删除。

An object has a 'type' that determines what it represents and what kind
of data it contains.  An object's type is fixed when it is created.
Types themselves are represented as objects; an object contains a
pointer to the corresponding type object.  The type itself has a type
pointer pointing to the object representing the type 'type', which
contains a pointer to itself!.
对象有一个“类型”，它决定了它代表什么以及它包含什么类型的数据。 
对象的类型在创建时就已确定。
类型本身被表示为对象； 一个对象包含一个指向相应类型对象的指针。
该类型本身有一个类型指针，指向表示类型“type”的对象，
该对象包含一个指向其自身的指针！

Objects do not float around in memory; once allocated an object keeps
the same size and address.  Objects that must hold variable-size data
can contain pointers to variable-size parts of the object.  Not all
objects of the same type have the same size; but the size cannot change
after allocation.  (These restrictions are made so a reference to an
object can be simply a pointer -- moving an object would require
updating all the pointers, and changing an object's size would require
moving it if there was another object right next to it.)
对象不会在内存中浮动； 一旦分配，对象就会保持相同的大小和地址。
必须保存可变大小数据的对象可以包含指向对象的可变大小部分的指针。
并非所有同一类型的对象都具有相同的大小； 但分配后大小不能改变。
（做出这些限制是为了使对对象的引用可以只是一个指针——移动对象需要更新所有指针，
并且如果对象旁边有另一个对象，则更改对象的大小将需要移动它。）

Objects are always accessed through pointers of the type 'PyObject *'.
The type 'PyObject' is a structure that only contains the reference count
and the type pointer.  The actual memory allocated for an object
contains other data that can only be accessed after casting the pointer
to a pointer to a longer structure type.  This longer type must start
with the reference count and type fields; the macro PyObject_HEAD should be
used for this (to accommodate for future changes).  The implementation
of a particular object type can cast the object pointer to the proper
type and back.
对象始终通过“PyObject *”类型的指针访问。
类型“PyObject”是一个仅包含引用计数和类型指针的结构。
为对象分配的实际内存包含其他数据，
只有在将指针转换为指向更长结构类型的指针后才能访问这些数据。
这个较长的类型必须以引用计数和类型字段开始； 
宏 PyObject_HEAD 应该用于此目的（以适应未来的更改）。
特定对象类型的实现可以将对象指针转换为正确的类型并返回。

A standard interface exists for objects that contain an array of items
whose size is determined when the object is allocated.
包含项目数组的对象存在一个标准接口，这些项目的大小在分配对象时确定。
*/

/* Py_DEBUG implies 意味着  Py_TRACE_REFS. */
#if defined(Py_DEBUG) && !defined(Py_TRACE_REFS)
#define Py_TRACE_REFS
#endif

/* Py_TRACE_REFS implies Py_REF_DEBUG. */
#if defined(Py_TRACE_REFS) && !defined(Py_REF_DEBUG)
#define Py_REF_DEBUG
#endif

#if defined(Py_LIMITED_API) && defined(Py_REF_DEBUG)
#error Py_LIMITED_API is incompatible with Py_DEBUG, Py_TRACE_REFS, and Py_REF_DEBUG
#endif


#ifdef Py_TRACE_REFS
/* Define pointers to support a doubly-linked list of all live heap objects. */
/* 定义指针以支持所有活动堆对象的双向链表。 */
#define _PyObject_HEAD_EXTRA            \
    struct _object *_ob_next;           \
    struct _object *_ob_prev;

#define _PyObject_EXTRA_INIT 0, 0,

#else
#define _PyObject_HEAD_EXTRA
#define _PyObject_EXTRA_INIT
#endif

/* PyObject_HEAD defines the initial segment of every PyObject. */
/* PyObject_HEAD 定义了每个 PyObject 的初始段。 */
#define PyObject_HEAD                   PyObject ob_base;

// 定义结构体 PyObject 的初始值 引用计数为1， 类型为传参的type， 
#define PyObject_HEAD_INIT(type)        \
    { _PyObject_EXTRA_INIT              \
    1, type },
// 定义结构体 PyVarObject 的初始值，引用计数为1， 类型为传参的type，元素的个数为传参size
#define PyVarObject_HEAD_INIT(type, size)       \
    { PyObject_HEAD_INIT(type) size },

/* PyObject_VAR_HEAD defines the initial segment of all variable-size
 * container objects.  These end with a declaration of an array with 1
 * element, but enough space is malloc'ed so that the array actually
 * has room for ob_size elements.  Note that ob_size is an element count,
 * not necessarily a byte count.
PyObject_VAR_HEAD 定义所有可变大小容器对象的初始段。
它们以具有 1 个元素的数组声明结束，但已分配了足够的空间，
以便数组实际上有空间容纳 ob_size 元素。
请注意，ob_size 是元素计数，不一定是字节计数。
 */
#define PyObject_VAR_HEAD      PyVarObject ob_base;
#define Py_INVALID_SIZE (Py_ssize_t)-1

/* Nothing is actually declared to be a PyObject, but every pointer to
 * a Python object can be cast to a PyObject*.  This is inheritance built
 * by hand.  Similarly every pointer to a variable-size Python object can,
 * in addition, be cast to PyVarObject*.
虽然没有任何东西被声明为 PyObject，但是指向 Python 对象的每个指针都可以被转换为 PyObject*。
这是手动构建的继承。类似地，除了指向可变大小的 Python 对象的指针外，每个指针还可以被转换为 PyVarObject*。
 */
// typedef 给已有的数据类型创建别名
typedef struct _object {
    _PyObject_HEAD_EXTRA
    /*引用计数，用于垃圾回收*/
    Py_ssize_t ob_refcnt;           
    /*
    指向对象类型的指针，用于标识对象的类型，运行时类型检查和类型特定的操作，
    每个对象有一个类型对象，定义了该对象的属性、行为、方法等。
    PyObject 对象到底是什么类型的，只有再调用的时候，通过ob_type来判断，即多态机制
    */
    struct _typeobject *ob_type;    
} PyObject; // 定长对象
/*
pyport.h中
Py_ssize_t 表示大小或索引。它在不同的平台上可以是不同的整数类型，
例如在 32 位系统上可能是 int 类型，在 64 位系统上可能是 long 类型

当有新的引用指向对象时，该计数会增加；当引用消失时，该计数会减少。
当引用计数为零时，对象将被自动销毁以释放内存
Python 中的每个对象都有一个与之关联的类型对象，该类型对象描述了对象的行为和属性。
*/

typedef struct {
    PyObject ob_base;
    Py_ssize_t ob_size; /* Number of items in variable part 可变部分的项目数 */
} PyVarObject; // 变长对象

#define Py_REFCNT(ob)           (((PyObject*)(ob))->ob_refcnt)  // 获取对象的引用计数
#define Py_TYPE(ob)             (((PyObject*)(ob))->ob_type)    // 获取对象的类型
#define Py_SIZE(ob)             (((PyVarObject*)(ob))->ob_size) // 获取对象的长度

#ifndef Py_LIMITED_API
/********************* String Literals ****************************************/
/* This structure helps managing static strings. The basic usage goes like this:
   Instead of doing

       r = PyObject_CallMethod(o, "foo", "args", ...);

   do

       _Py_IDENTIFIER(foo);
       ...
       r = _PyObject_CallMethodId(o, &PyId_foo, "args", ...);

   PyId_foo is a static variable, either on block level or file level. On first
   usage, the string "foo" is interned, and the structures are linked. On interpreter
   shutdown, all strings are released (through _PyUnicode_ClearStaticStrings).

   Alternatively, _Py_static_string allows choosing the variable name.
   _PyUnicode_FromId returns a borrowed reference to the interned string.
   _PyObject_{Get,Set,Has}AttrId are __getattr__ versions using _Py_Identifier*.
*/
typedef struct _Py_Identifier {
    struct _Py_Identifier *next;
    const char* string;
    PyObject *object;
} _Py_Identifier;

#define _Py_static_string_init(value) { .next = NULL, .string = value, .object = NULL }
#define _Py_static_string(varname, value)  static _Py_Identifier varname = _Py_static_string_init(value)
#define _Py_IDENTIFIER(varname) _Py_static_string(PyId_##varname, #varname)

#endif /* !Py_LIMITED_API */

/*
Type objects contain a string containing the type name (to help somewhat
in debugging), the allocation parameters (see PyObject_New() and
PyObject_NewVar()),
and methods for accessing objects of the type.  Methods are optional, a
nil pointer meaning that particular kind of access is not available for
this type.  The Py_DECREF() macro uses the tp_dealloc method without
checking for a nil pointer; it should always be implemented except if
the implementation can guarantee that the reference count will never
reach zero (e.g., for statically allocated type objects).

NB: the methods for certain type groups are now contained in separate
method blocks.

类型对象包含一个字符串，其中包含类型名称（在调试时有所帮助）、分配参数
（参见 PyObject_New() 和 PyObject_NewVar()）以及用于访问该类型对象的方法。
方法是可选的，一个空指针表示该类型的特定访问方式不可用。
Py_DECREF() 宏使用 tp_dealloc 方法而不检查空指针；它应该始终被实现，
除非实现可以保证引用计数永远不会达到零（例如，对于静态分配的类型对象）。

注：某些类型组的方法现在包含在单独的方法块中。
*/

typedef PyObject * (*unaryfunc)(PyObject *);
typedef PyObject * (*binaryfunc)(PyObject *, PyObject *);
typedef PyObject * (*ternaryfunc)(PyObject *, PyObject *, PyObject *);
typedef int (*inquiry)(PyObject *);
typedef Py_ssize_t (*lenfunc)(PyObject *);
typedef PyObject *(*ssizeargfunc)(PyObject *, Py_ssize_t);
typedef PyObject *(*ssizessizeargfunc)(PyObject *, Py_ssize_t, Py_ssize_t);
typedef int(*ssizeobjargproc)(PyObject *, Py_ssize_t, PyObject *);
typedef int(*ssizessizeobjargproc)(PyObject *, Py_ssize_t, Py_ssize_t, PyObject *);
typedef int(*objobjargproc)(PyObject *, PyObject *, PyObject *);

#ifndef Py_LIMITED_API
/* buffer interface */
typedef struct bufferinfo {
    void *buf;
    PyObject *obj;        /* owned reference */
    Py_ssize_t len;
    Py_ssize_t itemsize;  /* This is Py_ssize_t so it can be
                             pointed to by strides in simple case.*/
    int readonly;
    int ndim;
    char *format;
    Py_ssize_t *shape;
    Py_ssize_t *strides;
    Py_ssize_t *suboffsets;
    void *internal;
} Py_buffer;

typedef int (*getbufferproc)(PyObject *, Py_buffer *, int);
typedef void (*releasebufferproc)(PyObject *, Py_buffer *);

/* Maximum number of dimensions */
#define PyBUF_MAX_NDIM 64

/* Flags for getting buffers */
#define PyBUF_SIMPLE 0
#define PyBUF_WRITABLE 0x0001
/*  we used to include an E, backwards compatible alias  */
#define PyBUF_WRITEABLE PyBUF_WRITABLE
#define PyBUF_FORMAT 0x0004
#define PyBUF_ND 0x0008
#define PyBUF_STRIDES (0x0010 | PyBUF_ND)
#define PyBUF_C_CONTIGUOUS (0x0020 | PyBUF_STRIDES)
#define PyBUF_F_CONTIGUOUS (0x0040 | PyBUF_STRIDES)
#define PyBUF_ANY_CONTIGUOUS (0x0080 | PyBUF_STRIDES)
#define PyBUF_INDIRECT (0x0100 | PyBUF_STRIDES)

#define PyBUF_CONTIG (PyBUF_ND | PyBUF_WRITABLE)
#define PyBUF_CONTIG_RO (PyBUF_ND)

#define PyBUF_STRIDED (PyBUF_STRIDES | PyBUF_WRITABLE)
#define PyBUF_STRIDED_RO (PyBUF_STRIDES)

#define PyBUF_RECORDS (PyBUF_STRIDES | PyBUF_WRITABLE | PyBUF_FORMAT)
#define PyBUF_RECORDS_RO (PyBUF_STRIDES | PyBUF_FORMAT)

#define PyBUF_FULL (PyBUF_INDIRECT | PyBUF_WRITABLE | PyBUF_FORMAT)
#define PyBUF_FULL_RO (PyBUF_INDIRECT | PyBUF_FORMAT)


#define PyBUF_READ  0x100
#define PyBUF_WRITE 0x200

/* End buffer interface */
#endif /* Py_LIMITED_API */

typedef int (*objobjproc)(PyObject *, PyObject *);
typedef int (*visitproc)(PyObject *, void *);
typedef int (*traverseproc)(PyObject *, visitproc, void *);

#ifndef Py_LIMITED_API
typedef struct {
    /* Number implementations must check *both*
       arguments for proper type and implement the necessary conversions
       in the slot functions themselves. */
    /*
    unaryfunc 表示一元函数
    binaryfunc 表示二元函数 
    ternaryfunc 表示三元函数
    这些函数都是指向相应操作的函数指针。当自定义类型实现这些操作时，需要为这些函数指针赋值，指向实际的函数。
    不是所有方法都需要实现。对于不需要的操作，可以将函数指针设置为 NULL，表示该操作不适用于自定义的类型
    */
    binaryfunc nb_add;          // 加法
    binaryfunc nb_subtract;     // 减法
    binaryfunc nb_multiply;     // 乘法
    binaryfunc nb_remainder;    // 取余
    binaryfunc nb_divmod;       // 除法
    ternaryfunc nb_power;       // 指数
    unaryfunc nb_negative;      // 取负
    unaryfunc nb_positive;      // 取正
    unaryfunc nb_absolute;      // 取绝对值
    inquiry nb_bool;            // 转为bool值
    unaryfunc nb_invert;        // 按位取反
    binaryfunc nb_lshift;       // 左移
    binaryfunc nb_rshift;       // 右移
    binaryfunc nb_and;          // 与
    binaryfunc nb_xor;          // 异或
    binaryfunc nb_or;           // 或
    unaryfunc nb_int;           // 转换为整数
    void *nb_reserved;          /* 以前的 转为长整形 the slot formerly known as nb_long */
    unaryfunc nb_float;         // 转换为浮点数

    binaryfunc nb_inplace_add;          // 原地加法，不会创建新对象存储加法结果，节省内存
    binaryfunc nb_inplace_subtract;     // 原地减法
    binaryfunc nb_inplace_multiply;     // 原地乘法
    binaryfunc nb_inplace_remainder;    // 原地取余
    ternaryfunc nb_inplace_power;       // 原地指数
    binaryfunc nb_inplace_lshift;       // 原地左移
    binaryfunc nb_inplace_rshift;       // 原地右移
    binaryfunc nb_inplace_and;          // 原地与
    binaryfunc nb_inplace_xor;          // 原地异或
    binaryfunc nb_inplace_or;           // 原地或

    binaryfunc nb_floor_divide;         // 地板除法，使用//符号，向下取整 4//3 -> 1
    binaryfunc nb_true_divide;          // 真除法，使用/符号，结果一般为浮点型 4/3 -> 1.3333333333333333
    binaryfunc nb_inplace_floor_divide; // 原地地板除法
    binaryfunc nb_inplace_true_divide;  // 原地真除法

    unaryfunc nb_index;                 // 返回用于切片或序列索引的整数

    binaryfunc nb_matrix_multiply;      // 矩阵乘法
    binaryfunc nb_inplace_matrix_multiply;  // 原地矩阵乘法
} PyNumberMethods;

typedef struct {
    // 序列对象的函数
    lenfunc sq_length;              // 序列长度
    binaryfunc sq_concat;           // 连接序列
    ssizeargfunc sq_repeat;         // 重复序列
    ssizeargfunc sq_item;           // 获取序列中指定位置的元素
    void *was_sq_slice;             // 保留字段，曾经用于存储指向切片操作函数的指针
    ssizeobjargproc sq_ass_item;    // 设置序列中指定位置的元素
    void *was_sq_ass_slice;         //
    objobjproc sq_contains;         // 检查序列是否包含某个元素

    binaryfunc sq_inplace_concat;   // 原地连接另一个序列到当前序列
    ssizeargfunc sq_inplace_repeat; // 原地重复序列指定的次数
} PySequenceMethods;

typedef struct {
    // 映射对象的函数
    lenfunc mp_length;              // 返回映射的长度（即键值对的数量）
    binaryfunc mp_subscript;        // 获取映射中指定键对应的值
    objobjargproc mp_ass_subscript; // 设置映射中指定键对应的值
} PyMappingMethods;

typedef struct {
    // 异步对象的函数
    unaryfunc am_await;     // 
    unaryfunc am_aiter;     // 
    unaryfunc am_anext;     //
} PyAsyncMethods;

typedef struct {
     getbufferproc bf_getbuffer;
     releasebufferproc bf_releasebuffer;
} PyBufferProcs;
#endif /* Py_LIMITED_API */

typedef void (*freefunc)(void *);
typedef void (*destructor)(PyObject *);
#ifndef Py_LIMITED_API
/* We can't provide a full compile-time check that limited-API
   users won't implement tp_print. However, not defining printfunc
   and making tp_print of a different function pointer type
   should at least cause a warning in most cases. */
typedef int (*printfunc)(PyObject *, FILE *, int);
#endif
typedef PyObject *(*getattrfunc)(PyObject *, char *);
typedef PyObject *(*getattrofunc)(PyObject *, PyObject *);
typedef int (*setattrfunc)(PyObject *, char *, PyObject *);
typedef int (*setattrofunc)(PyObject *, PyObject *, PyObject *);
typedef PyObject *(*reprfunc)(PyObject *);
typedef Py_hash_t (*hashfunc)(PyObject *);
typedef PyObject *(*richcmpfunc) (PyObject *, PyObject *, int);
typedef PyObject *(*getiterfunc) (PyObject *);
typedef PyObject *(*iternextfunc) (PyObject *);
typedef PyObject *(*descrgetfunc) (PyObject *, PyObject *, PyObject *);
typedef int (*descrsetfunc) (PyObject *, PyObject *, PyObject *);
typedef int (*initproc)(PyObject *, PyObject *, PyObject *);
typedef PyObject *(*newfunc)(struct _typeobject *, PyObject *, PyObject *);
typedef PyObject *(*allocfunc)(struct _typeobject *, Py_ssize_t);

// PyTypeObject 类型对象
#ifdef Py_LIMITED_API
typedef struct _typeobject PyTypeObject; /* opaque */
#else
typedef struct _typeobject {
    /* 
    类型对象也是可变长的对象，也有对应的类型，
    也就是说所有的对象都有类型，包括类型对象本身也有类型
    */
    PyObject_VAR_HEAD               //  类型对象的头部，引用计数和类型标记
    const char *tp_name;            // 类型名称
    Py_ssize_t tp_basicsize, tp_itemsize; // tp_basicsize 基本大小  tp_itemsize可变长对象的每个元素的大小

    /* Methods to implement standard operations */

    destructor tp_dealloc;          // 对象销毁函数指针，用于释放对象所占用的内存
    printfunc tp_print;             // 用于打印类型对象的函数
    getattrfunc tp_getattr;         // 获取属性的函数
    setattrfunc tp_setattr;         // 设置属性的函数
    PyAsyncMethods *tp_as_async; /* formerly known as tp_compare (Python 2)
                                    or tp_reserved (Python 3) */
    reprfunc tp_repr;               // 生成类型对象字符串表示的函数

    /* Method suites for standard classes */

    PyNumberMethods *tp_as_number;      // 数值对象的方法集
    PySequenceMethods *tp_as_sequence;  // 序列对象的方法集
    PyMappingMethods *tp_as_mapping;    // 映射相关的方法集

    /* More standard operations (here for binary compatibility) */

    hashfunc tp_hash;                   
    ternaryfunc tp_call;
    reprfunc tp_str;
    getattrofunc tp_getattro;
    setattrofunc tp_setattro;

    /* Functions to access object as input/output buffer */
    PyBufferProcs *tp_as_buffer;

    /* Flags to define presence of optional/expanded features */
    unsigned long tp_flags;         // 类型的标志，用于描述该类型的一些特性，比如是否是可变类型、是否是序列类型等。

    const char *tp_doc; /* Documentation string */

    /* Assigned meaning in release 2.0 */
    /* call function for all accessible objects */
    traverseproc tp_traverse;

    /* delete references to contained objects */
    inquiry tp_clear;

    /* Assigned meaning in release 2.1 */
    /* rich comparisons */
    richcmpfunc tp_richcompare;

    /* weak reference enabler */
    Py_ssize_t tp_weaklistoffset;

    /* Iterators */
    getiterfunc tp_iter;
    iternextfunc tp_iternext;

    /* Attribute descriptor and subclassing stuff */
    struct PyMethodDef *tp_methods;
    struct PyMemberDef *tp_members;
    struct PyGetSetDef *tp_getset;
    struct _typeobject *tp_base;
    PyObject *tp_dict;              // 类型对象的字典，用于存储类型的属性
    descrgetfunc tp_descr_get;
    descrsetfunc tp_descr_set;
    Py_ssize_t tp_dictoffset;
    initproc tp_init;
    allocfunc tp_alloc;
    newfunc tp_new;
    freefunc tp_free; /* Low-level free-memory routine */
    inquiry tp_is_gc; /* For PyObject_IS_GC */
    PyObject *tp_bases;
    PyObject *tp_mro; /* method resolution order */
    PyObject *tp_cache;
    PyObject *tp_subclasses;
    PyObject *tp_weaklist;
    destructor tp_del;

    /* Type attribute cache version tag. Added in version 2.6 */
    unsigned int tp_version_tag;

    destructor tp_finalize;

#ifdef COUNT_ALLOCS
    /* these must be last and never explicitly initialized */
    Py_ssize_t tp_allocs;
    Py_ssize_t tp_frees;
    Py_ssize_t tp_maxalloc;
    struct _typeobject *tp_prev;
    struct _typeobject *tp_next;
#endif
} PyTypeObject;
#endif

typedef struct{
    int slot;    /* slot id, see below */
    void *pfunc; /* function pointer */
} PyType_Slot;

typedef struct{
    const char* name;
    int basicsize;
    int itemsize;
    unsigned int flags;
    PyType_Slot *slots; /* terminated by slot==0. */
} PyType_Spec;

PyAPI_FUNC(PyObject*) PyType_FromSpec(PyType_Spec*);
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyObject*) PyType_FromSpecWithBases(PyType_Spec*, PyObject*);
#endif
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03040000
PyAPI_FUNC(void*) PyType_GetSlot(PyTypeObject*, int);
#endif

#ifndef Py_LIMITED_API
/* The *real* layout of a type object when allocated on the heap */
typedef struct _heaptypeobject {
    /* Note: there's a dependency on the order of these members
       in slotptr() in typeobject.c . */
    PyTypeObject ht_type;
    PyAsyncMethods as_async;
    PyNumberMethods as_number;
    PyMappingMethods as_mapping;
    PySequenceMethods as_sequence; /* as_sequence comes after as_mapping,
                                      so that the mapping wins when both
                                      the mapping and the sequence define
                                      a given operator (e.g. __getitem__).
                                      see add_operators() in typeobject.c . */
    PyBufferProcs as_buffer;
    PyObject *ht_name, *ht_slots, *ht_qualname;
    struct _dictkeysobject *ht_cached_keys;
    /* here are optional user slots, followed by the members. */
} PyHeapTypeObject;

/* access macro to the members which are floating "behind" the object */
#define PyHeapType_GET_MEMBERS(etype) \
    ((PyMemberDef *)(((char *)etype) + Py_TYPE(etype)->tp_basicsize))
#endif

/* Generic type check */
PyAPI_FUNC(int) PyType_IsSubtype(PyTypeObject *, PyTypeObject *);
#define PyObject_TypeCheck(ob, tp) \
    (Py_TYPE(ob) == (tp) || PyType_IsSubtype(Py_TYPE(ob), (tp)))

PyAPI_DATA(PyTypeObject) PyType_Type; /* built-in 'type' */
PyAPI_DATA(PyTypeObject) PyBaseObject_Type; /* built-in 'object' */
PyAPI_DATA(PyTypeObject) PySuper_Type; /* built-in 'super' */

PyAPI_FUNC(unsigned long) PyType_GetFlags(PyTypeObject*);

#define PyType_Check(op) \
    PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_TYPE_SUBCLASS)
#define PyType_CheckExact(op) (Py_TYPE(op) == &PyType_Type)

PyAPI_FUNC(int) PyType_Ready(PyTypeObject *);
PyAPI_FUNC(PyObject *) PyType_GenericAlloc(PyTypeObject *, Py_ssize_t);
PyAPI_FUNC(PyObject *) PyType_GenericNew(PyTypeObject *,
                                               PyObject *, PyObject *);
#ifndef Py_LIMITED_API
PyAPI_FUNC(const char *) _PyType_Name(PyTypeObject *);
PyAPI_FUNC(PyObject *) _PyType_Lookup(PyTypeObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyType_LookupId(PyTypeObject *, _Py_Identifier *);
PyAPI_FUNC(PyObject *) _PyObject_LookupSpecial(PyObject *, _Py_Identifier *);
PyAPI_FUNC(PyTypeObject *) _PyType_CalculateMetaclass(PyTypeObject *, PyObject *);
#endif
PyAPI_FUNC(unsigned int) PyType_ClearCache(void);
PyAPI_FUNC(void) PyType_Modified(PyTypeObject *);

#ifndef Py_LIMITED_API
PyAPI_FUNC(PyObject *) _PyType_GetDocFromInternalDoc(const char *, const char *);
PyAPI_FUNC(PyObject *) _PyType_GetTextSignatureFromInternalDoc(const char *, const char *);
#endif

/* Generic operations on objects */
#ifndef Py_LIMITED_API
struct _Py_Identifier;
PyAPI_FUNC(int) PyObject_Print(PyObject *, FILE *, int);
PyAPI_FUNC(void) _Py_BreakPoint(void);
PyAPI_FUNC(void) _PyObject_Dump(PyObject *);
PyAPI_FUNC(int) _PyObject_IsFreed(PyObject *);
#endif
PyAPI_FUNC(PyObject *) PyObject_Repr(PyObject *);
PyAPI_FUNC(PyObject *) PyObject_Str(PyObject *);
PyAPI_FUNC(PyObject *) PyObject_ASCII(PyObject *);
PyAPI_FUNC(PyObject *) PyObject_Bytes(PyObject *);
PyAPI_FUNC(PyObject *) PyObject_RichCompare(PyObject *, PyObject *, int);
PyAPI_FUNC(int) PyObject_RichCompareBool(PyObject *, PyObject *, int);
PyAPI_FUNC(PyObject *) PyObject_GetAttrString(PyObject *, const char *);
PyAPI_FUNC(int) PyObject_SetAttrString(PyObject *, const char *, PyObject *);
PyAPI_FUNC(int) PyObject_HasAttrString(PyObject *, const char *);
PyAPI_FUNC(PyObject *) PyObject_GetAttr(PyObject *, PyObject *);
PyAPI_FUNC(int) PyObject_SetAttr(PyObject *, PyObject *, PyObject *);
PyAPI_FUNC(int) PyObject_HasAttr(PyObject *, PyObject *);
#ifndef Py_LIMITED_API
PyAPI_FUNC(int) _PyObject_IsAbstract(PyObject *);
PyAPI_FUNC(PyObject *) _PyObject_GetAttrId(PyObject *, struct _Py_Identifier *);
PyAPI_FUNC(int) _PyObject_SetAttrId(PyObject *, struct _Py_Identifier *, PyObject *);
PyAPI_FUNC(int) _PyObject_HasAttrId(PyObject *, struct _Py_Identifier *);
/* Replacements of PyObject_GetAttr() and _PyObject_GetAttrId() which
   don't raise AttributeError.

   Return 1 and set *result != NULL if an attribute is found.
   Return 0 and set *result == NULL if an attribute is not found;
   an AttributeError is silenced.
   Return -1 and set *result == NULL if an error other than AttributeError
   is raised.
*/
PyAPI_FUNC(int) _PyObject_LookupAttr(PyObject *, PyObject *, PyObject **);
PyAPI_FUNC(int) _PyObject_LookupAttrId(PyObject *, struct _Py_Identifier *, PyObject **);
PyAPI_FUNC(PyObject **) _PyObject_GetDictPtr(PyObject *);
#endif
PyAPI_FUNC(PyObject *) PyObject_SelfIter(PyObject *);
#ifndef Py_LIMITED_API
PyAPI_FUNC(PyObject *) _PyObject_NextNotImplemented(PyObject *);
#endif
PyAPI_FUNC(PyObject *) PyObject_GenericGetAttr(PyObject *, PyObject *);
PyAPI_FUNC(int) PyObject_GenericSetAttr(PyObject *,
                                              PyObject *, PyObject *);
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(int) PyObject_GenericSetDict(PyObject *, PyObject *, void *);
#endif
PyAPI_FUNC(Py_hash_t) PyObject_Hash(PyObject *);
PyAPI_FUNC(Py_hash_t) PyObject_HashNotImplemented(PyObject *);
PyAPI_FUNC(int) PyObject_IsTrue(PyObject *);
PyAPI_FUNC(int) PyObject_Not(PyObject *);
PyAPI_FUNC(int) PyCallable_Check(PyObject *);

PyAPI_FUNC(void) PyObject_ClearWeakRefs(PyObject *);
#ifndef Py_LIMITED_API
PyAPI_FUNC(void) PyObject_CallFinalizer(PyObject *);
PyAPI_FUNC(int) PyObject_CallFinalizerFromDealloc(PyObject *);
#endif

#ifndef Py_LIMITED_API
/* Same as PyObject_Generic{Get,Set}Attr, but passing the attributes
   dict as the last parameter. */
PyAPI_FUNC(PyObject *)
_PyObject_GenericGetAttrWithDict(PyObject *, PyObject *, PyObject *, int);
PyAPI_FUNC(int)
_PyObject_GenericSetAttrWithDict(PyObject *, PyObject *,
                                 PyObject *, PyObject *);
#endif /* !Py_LIMITED_API */

/* Helper to look up a builtin object */
#ifndef Py_LIMITED_API
PyAPI_FUNC(PyObject *)
_PyObject_GetBuiltin(const char *name);
#endif

/* PyObject_Dir(obj) acts like Python builtins.dir(obj), returning a
   list of strings.  PyObject_Dir(NULL) is like builtins.dir(),
   returning the names of the current locals.  In this case, if there are
   no current locals, NULL is returned, and PyErr_Occurred() is false.
*/
PyAPI_FUNC(PyObject *) PyObject_Dir(PyObject *);


/* Helpers for printing recursive container types */
PyAPI_FUNC(int) Py_ReprEnter(PyObject *);
PyAPI_FUNC(void) Py_ReprLeave(PyObject *);

/* Flag bits for printing: */
#define Py_PRINT_RAW    1       /* No string quotes etc. */

/*
`Type flags (tp_flags)

These flags are used to extend the type structure in a backwards-compatible
fashion. Extensions can use the flags to indicate (and test) when a given
type structure contains a new feature. The Python core will use these when
introducing new functionality between major revisions (to avoid mid-version
changes in the PYTHON_API_VERSION).

Arbitration of the flag bit positions will need to be coordinated among
all extension writers who publicly release their extensions (this will
be fewer than you might expect!)..

Most flags were removed as of Python 3.0 to make room for new flags.  (Some
flags are not for backwards compatibility but to indicate the presence of an
optional feature; these flags remain of course.)

Type definitions should use Py_TPFLAGS_DEFAULT for their tp_flags value.

Code can use PyType_HasFeature(type_ob, flag_value) to test whether the
given type object has a specified feature.
*/

/* Set if the type object is dynamically allocated */
#define Py_TPFLAGS_HEAPTYPE (1UL << 9)

/* Set if the type allows subclassing */
#define Py_TPFLAGS_BASETYPE (1UL << 10)

/* Set if the type is 'ready' -- fully initialized */
#define Py_TPFLAGS_READY (1UL << 12)

/* Set while the type is being 'readied', to prevent recursive ready calls */
#define Py_TPFLAGS_READYING (1UL << 13)

/* Objects support garbage collection (see objimp.h) */
#define Py_TPFLAGS_HAVE_GC (1UL << 14)

/* These two bits are preserved for Stackless Python, next after this is 17 */
#ifdef STACKLESS
#define Py_TPFLAGS_HAVE_STACKLESS_EXTENSION (3UL << 15)
#else
#define Py_TPFLAGS_HAVE_STACKLESS_EXTENSION 0
#endif

/* Objects support type attribute cache */
#define Py_TPFLAGS_HAVE_VERSION_TAG   (1UL << 18)
#define Py_TPFLAGS_VALID_VERSION_TAG  (1UL << 19)

/* Type is abstract and cannot be instantiated */
#define Py_TPFLAGS_IS_ABSTRACT (1UL << 20)

/* These flags are used to determine if a type is a subclass. */
#define Py_TPFLAGS_LONG_SUBCLASS        (1UL << 24)
#define Py_TPFLAGS_LIST_SUBCLASS        (1UL << 25)
#define Py_TPFLAGS_TUPLE_SUBCLASS       (1UL << 26)
#define Py_TPFLAGS_BYTES_SUBCLASS       (1UL << 27)
#define Py_TPFLAGS_UNICODE_SUBCLASS     (1UL << 28)
#define Py_TPFLAGS_DICT_SUBCLASS        (1UL << 29)
#define Py_TPFLAGS_BASE_EXC_SUBCLASS    (1UL << 30)
#define Py_TPFLAGS_TYPE_SUBCLASS        (1UL << 31)

#define Py_TPFLAGS_DEFAULT  ( \
                 Py_TPFLAGS_HAVE_STACKLESS_EXTENSION | \
                 Py_TPFLAGS_HAVE_VERSION_TAG | \
                0)

/* NOTE: The following flags reuse lower bits (removed as part of the
 * Python 3.0 transition). */

/* Type structure has tp_finalize member (3.4) */
#define Py_TPFLAGS_HAVE_FINALIZE (1UL << 0)

#ifdef Py_LIMITED_API
#define PyType_HasFeature(t,f)  ((PyType_GetFlags(t) & (f)) != 0)
#else
#define PyType_HasFeature(t,f)  (((t)->tp_flags & (f)) != 0)
#endif
#define PyType_FastSubclass(t,f)  PyType_HasFeature(t,f)


/*
The macros Py_INCREF(op) and Py_DECREF(op) are used to increment or decrement
reference counts.  Py_DECREF calls the object's deallocator function when
the refcount falls to 0; for
objects that don't contain references to other objects or heap memory
this can be the standard function free().  Both macros can be used
wherever a void expression is allowed.  The argument must not be a
NULL pointer.  If it may be NULL, use Py_XINCREF/Py_XDECREF instead.
The macro _Py_NewReference(op) initialize reference counts to 1, and
in special builds (Py_REF_DEBUG, Py_TRACE_REFS) performs additional
bookkeeping appropriate to the special build.

We assume that the reference count field can never overflow; this can
be proven when the size of the field is the same as the pointer size, so
we ignore the possibility.  Provided a C int is at least 32 bits (which
is implicitly assumed in many parts of this code), that's enough for
about 2**31 references to an object.

XXX The following became out of date in Python 2.2, but I'm not sure
XXX what the full truth is now.  Certainly, heap-allocated type objects
XXX can and should be deallocated.
Type objects should never be deallocated; the type pointer in an object
is not considered to be a reference to the type object, to save
complications in the deallocation function.  (This is actually a
decision that's up to the implementer of each new type so if you want,
you can count such references to the type object.)
*/

/* First define a pile of simple helper macros, one set per special
 * build symbol.  These either expand to the obvious things, or to
 * nothing at all when the special mode isn't in effect.  The main
 * macros can later be defined just once then, yet expand to different
 * things depending on which special build options are and aren't in effect.
 * Trust me <wink>:  while painful, this is 20x easier to understand than,
 * e.g, defining _Py_NewReference five different times in a maze of nested
 * #ifdefs (we used to do that -- it was impenetrable).
 */
#ifdef Py_REF_DEBUG
PyAPI_DATA(Py_ssize_t) _Py_RefTotal;
PyAPI_FUNC(void) _Py_NegativeRefcount(const char *fname,
                                            int lineno, PyObject *op);
PyAPI_FUNC(Py_ssize_t) _Py_GetRefTotal(void);
#define _Py_INC_REFTOTAL        _Py_RefTotal++
#define _Py_DEC_REFTOTAL        _Py_RefTotal--
#define _Py_REF_DEBUG_COMMA     ,
#define _Py_CHECK_REFCNT(OP)                                    \
{       if (((PyObject*)OP)->ob_refcnt < 0)                             \
                _Py_NegativeRefcount(__FILE__, __LINE__,        \
                                     (PyObject *)(OP));         \
}
/* Py_REF_DEBUG also controls the display of refcounts and memory block
 * allocations at the interactive prompt and at interpreter shutdown
 */
PyAPI_FUNC(void) _PyDebug_PrintTotalRefs(void);
#else
#define _Py_INC_REFTOTAL
#define _Py_DEC_REFTOTAL
#define _Py_REF_DEBUG_COMMA
#define _Py_CHECK_REFCNT(OP)    /* a semicolon */;
#endif /* Py_REF_DEBUG */

#ifdef COUNT_ALLOCS
PyAPI_FUNC(void) inc_count(PyTypeObject *);
PyAPI_FUNC(void) dec_count(PyTypeObject *);
#define _Py_INC_TPALLOCS(OP)    inc_count(Py_TYPE(OP))
#define _Py_INC_TPFREES(OP)     dec_count(Py_TYPE(OP))
#define _Py_DEC_TPFREES(OP)     Py_TYPE(OP)->tp_frees--
#define _Py_COUNT_ALLOCS_COMMA  ,
#else
#define _Py_INC_TPALLOCS(OP)
#define _Py_INC_TPFREES(OP)
#define _Py_DEC_TPFREES(OP)
#define _Py_COUNT_ALLOCS_COMMA
#endif /* COUNT_ALLOCS */

#ifdef Py_TRACE_REFS
/* Py_TRACE_REFS is such major surgery that we call external routines. */
PyAPI_FUNC(void) _Py_NewReference(PyObject *);
PyAPI_FUNC(void) _Py_ForgetReference(PyObject *);
PyAPI_FUNC(void) _Py_Dealloc(PyObject *);
PyAPI_FUNC(void) _Py_PrintReferences(FILE *);
PyAPI_FUNC(void) _Py_PrintReferenceAddresses(FILE *);
PyAPI_FUNC(void) _Py_AddToAllObjects(PyObject *, int force);

#else
/* Without Py_TRACE_REFS, there's little enough to do that we expand code
 * inline.
 */
#define _Py_NewReference(op) (                          \
    _Py_INC_TPALLOCS(op) _Py_COUNT_ALLOCS_COMMA         \
    _Py_INC_REFTOTAL  _Py_REF_DEBUG_COMMA               \
    Py_REFCNT(op) = 1)     // 创建新对象的时候，引用计数会被初始化为1                             

#define _Py_ForgetReference(op) _Py_INC_TPFREES(op)

#ifdef Py_LIMITED_API
PyAPI_FUNC(void) _Py_Dealloc(PyObject *);
#else
#define _Py_Dealloc(op) (                               \
    _Py_INC_TPFREES(op) _Py_COUNT_ALLOCS_COMMA          \
    (*Py_TYPE(op)->tp_dealloc)((PyObject *)(op)))
#endif
#endif /* !Py_TRACE_REFS */

#define Py_INCREF(op) (                         \
    _Py_INC_REFTOTAL  _Py_REF_DEBUG_COMMA       \
    ((PyObject *)(op))->ob_refcnt++)

#define Py_DECREF(op)                                   \
    do {                                                \
        PyObject *_py_decref_tmp = (PyObject *)(op);    \
        if (_Py_DEC_REFTOTAL  _Py_REF_DEBUG_COMMA       \
        --(_py_decref_tmp)->ob_refcnt != 0)             \
            _Py_CHECK_REFCNT(_py_decref_tmp)            \
        else                                            \
            _Py_Dealloc(_py_decref_tmp);                \
    } while (0)

/* Safely decref `op` and set `op` to NULL, especially useful in tp_clear
 * and tp_dealloc implementations.
 *
 * Note that "the obvious" code can be deadly:
 *
 *     Py_XDECREF(op);
 *     op = NULL;
 *
 * Typically, `op` is something like self->containee, and `self` is done
 * using its `containee` member.  In the code sequence above, suppose
 * `containee` is non-NULL with a refcount of 1.  Its refcount falls to
 * 0 on the first line, which can trigger an arbitrary amount of code,
 * possibly including finalizers (like __del__ methods or weakref callbacks)
 * coded in Python, which in turn can release the GIL and allow other threads
 * to run, etc.  Such code may even invoke methods of `self` again, or cause
 * cyclic gc to trigger, but-- oops! --self->containee still points to the
 * object being torn down, and it may be in an insane state while being torn
 * down.  This has in fact been a rich historic source of miserable (rare &
 * hard-to-diagnose) segfaulting (and other) bugs.
 *
 * The safe way is:
 *
 *      Py_CLEAR(op);
 *
 * That arranges to set `op` to NULL _before_ decref'ing, so that any code
 * triggered as a side-effect of `op` getting torn down no longer believes
 * `op` points to a valid object.
 *
 * There are cases where it's safe to use the naive code, but they're brittle.
 * For example, if `op` points to a Python integer, you know that destroying
 * one of those can't cause problems -- but in part that relies on that
 * Python integers aren't currently weakly referencable.  Best practice is
 * to use Py_CLEAR() even if you can't think of a reason for why you need to.
 */
#define Py_CLEAR(op)                            \
    do {                                        \
        PyObject *_py_tmp = (PyObject *)(op);   \
        if (_py_tmp != NULL) {                  \
            (op) = NULL;                        \
            Py_DECREF(_py_tmp);                 \
        }                                       \
    } while (0)

/* Macros to use in case the object pointer may be NULL: */
#define Py_XINCREF(op)                                \
    do {                                              \
        PyObject *_py_xincref_tmp = (PyObject *)(op); \
        if (_py_xincref_tmp != NULL)                  \
            Py_INCREF(_py_xincref_tmp);               \
    } while (0)

#define Py_XDECREF(op)                                \
    do {                                              \
        PyObject *_py_xdecref_tmp = (PyObject *)(op); \
        if (_py_xdecref_tmp != NULL)                  \
            Py_DECREF(_py_xdecref_tmp);               \
    } while (0)

#ifndef Py_LIMITED_API
/* Safely decref `op` and set `op` to `op2`.
 *
 * As in case of Py_CLEAR "the obvious" code can be deadly:
 *
 *     Py_DECREF(op);
 *     op = op2;
 *
 * The safe way is:
 *
 *      Py_SETREF(op, op2);
 *
 * That arranges to set `op` to `op2` _before_ decref'ing, so that any code
 * triggered as a side-effect of `op` getting torn down no longer believes
 * `op` points to a valid object.
 *
 * Py_XSETREF is a variant of Py_SETREF that uses Py_XDECREF instead of
 * Py_DECREF.
 */

#define Py_SETREF(op, op2)                      \
    do {                                        \
        PyObject *_py_tmp = (PyObject *)(op);   \
        (op) = (op2);                           \
        Py_DECREF(_py_tmp);                     \
    } while (0)

#define Py_XSETREF(op, op2)                     \
    do {                                        \
        PyObject *_py_tmp = (PyObject *)(op);   \
        (op) = (op2);                           \
        Py_XDECREF(_py_tmp);                    \
    } while (0)

#endif /* ifndef Py_LIMITED_API */

/*
These are provided as conveniences to Python runtime embedders, so that
they can have object code that is not dependent on Python compilation flags.
*/
PyAPI_FUNC(void) Py_IncRef(PyObject *);
PyAPI_FUNC(void) Py_DecRef(PyObject *);

#ifndef Py_LIMITED_API
PyAPI_DATA(PyTypeObject) _PyNone_Type;
PyAPI_DATA(PyTypeObject) _PyNotImplemented_Type;
#endif /* !Py_LIMITED_API */

/*
_Py_NoneStruct is an object of undefined type which can be used in contexts
where NULL (nil) is not suitable (since NULL often means 'error').

Don't forget to apply Py_INCREF() when returning this value!!!
*/
PyAPI_DATA(PyObject) _Py_NoneStruct; /* Don't use this directly */
#define Py_None (&_Py_NoneStruct)

/* Macro for returning Py_None from a function */
#define Py_RETURN_NONE return Py_INCREF(Py_None), Py_None

/*
Py_NotImplemented is a singleton used to signal that an operation is
not implemented for a given type combination.
*/
PyAPI_DATA(PyObject) _Py_NotImplementedStruct; /* Don't use this directly */
#define Py_NotImplemented (&_Py_NotImplementedStruct)

/* Macro for returning Py_NotImplemented from a function */
#define Py_RETURN_NOTIMPLEMENTED \
    return Py_INCREF(Py_NotImplemented), Py_NotImplemented

/* Rich comparison opcodes */
#define Py_LT 0
#define Py_LE 1
#define Py_EQ 2
#define Py_NE 3
#define Py_GT 4
#define Py_GE 5

/*
 * Macro for implementing rich comparisons
 *
 * Needs to be a macro because any C-comparable type can be used.
 */
#define Py_RETURN_RICHCOMPARE(val1, val2, op)                               \
    do {                                                                    \
        switch (op) {                                                       \
        case Py_EQ: if ((val1) == (val2)) Py_RETURN_TRUE; Py_RETURN_FALSE;  \
        case Py_NE: if ((val1) != (val2)) Py_RETURN_TRUE; Py_RETURN_FALSE;  \
        case Py_LT: if ((val1) < (val2)) Py_RETURN_TRUE; Py_RETURN_FALSE;   \
        case Py_GT: if ((val1) > (val2)) Py_RETURN_TRUE; Py_RETURN_FALSE;   \
        case Py_LE: if ((val1) <= (val2)) Py_RETURN_TRUE; Py_RETURN_FALSE;  \
        case Py_GE: if ((val1) >= (val2)) Py_RETURN_TRUE; Py_RETURN_FALSE;  \
        default:                                                            \
            Py_UNREACHABLE();                                               \
        }                                                                   \
    } while (0)

#ifndef Py_LIMITED_API
/* Maps Py_LT to Py_GT, ..., Py_GE to Py_LE.
 * Defined in object.c.
 */
PyAPI_DATA(int) _Py_SwappedOp[];
#endif /* !Py_LIMITED_API */


/*
More conventions
================

Argument Checking
-----------------

Functions that take objects as arguments normally don't check for nil
arguments, but they do check the type of the argument, and return an
error if the function doesn't apply to the type.

Failure Modes
-------------

Functions may fail for a variety of reasons, including running out of
memory.  This is communicated to the caller in two ways: an error string
is set (see errors.h), and the function result differs: functions that
normally return a pointer return NULL for failure, functions returning
an integer return -1 (which could be a legal return value too!), and
other functions return 0 for success and -1 for failure.
Callers should always check for errors before using the result.  If
an error was set, the caller must either explicitly clear it, or pass
the error on to its caller.

Reference Counts
----------------

It takes a while to get used to the proper usage of reference counts.

Functions that create an object set the reference count to 1; such new
objects must be stored somewhere or destroyed again with Py_DECREF().
Some functions that 'store' objects, such as PyTuple_SetItem() and
PyList_SetItem(),
don't increment the reference count of the object, since the most
frequent use is to store a fresh object.  Functions that 'retrieve'
objects, such as PyTuple_GetItem() and PyDict_GetItemString(), also
don't increment
the reference count, since most frequently the object is only looked at
quickly.  Thus, to retrieve an object and store it again, the caller
must call Py_INCREF() explicitly.

NOTE: functions that 'consume' a reference count, like
PyList_SetItem(), consume the reference even if the object wasn't
successfully stored, to simplify error handling.

It seems attractive to make other functions that take an object as
argument consume a reference count; however, this may quickly get
confusing (even the current practice is already confusing).  Consider
it carefully, it may save lots of calls to Py_INCREF() and Py_DECREF() at
times.
*/


/* Trashcan mechanism, thanks to Christian Tismer.

When deallocating a container object, it's possible to trigger an unbounded
chain of deallocations, as each Py_DECREF in turn drops the refcount on "the
next" object in the chain to 0.  This can easily lead to stack faults, and
especially in threads (which typically have less stack space to work with).

A container object that participates in cyclic gc can avoid this by
bracketing the body of its tp_dealloc function with a pair of macros:

static void
mytype_dealloc(mytype *p)
{
    ... declarations go here ...

    PyObject_GC_UnTrack(p);        // must untrack first
    Py_TRASHCAN_SAFE_BEGIN(p)
    ... The body of the deallocator goes here, including all calls ...
    ... to Py_DECREF on contained objects.                         ...
    Py_TRASHCAN_SAFE_END(p)
}

CAUTION:  Never return from the middle of the body!  If the body needs to
"get out early", put a label immediately before the Py_TRASHCAN_SAFE_END
call, and goto it.  Else the call-depth counter (see below) will stay
above 0 forever, and the trashcan will never get emptied.

How it works:  The BEGIN macro increments a call-depth counter.  So long
as this counter is small, the body of the deallocator is run directly without
further ado.  But if the counter gets large, it instead adds p to a list of
objects to be deallocated later, skips the body of the deallocator, and
resumes execution after the END macro.  The tp_dealloc routine then returns
without deallocating anything (and so unbounded call-stack depth is avoided).

When the call stack finishes unwinding again, code generated by the END macro
notices this, and calls another routine to deallocate all the objects that
may have been added to the list of deferred deallocations.  In effect, a
chain of N deallocations is broken into (N-1)/(PyTrash_UNWIND_LEVEL-1) pieces,
with the call stack never exceeding a depth of PyTrash_UNWIND_LEVEL.
*/

#ifndef Py_LIMITED_API
/* This is the old private API, invoked by the macros before 3.2.4.
   Kept for binary compatibility of extensions using the stable ABI. */
PyAPI_FUNC(void) _PyTrash_deposit_object(PyObject*);
PyAPI_FUNC(void) _PyTrash_destroy_chain(void);
#endif /* !Py_LIMITED_API */

/* The new thread-safe private API, invoked by the macros below. */
PyAPI_FUNC(void) _PyTrash_thread_deposit_object(PyObject*);
PyAPI_FUNC(void) _PyTrash_thread_destroy_chain(void);

#define PyTrash_UNWIND_LEVEL 50

#define Py_TRASHCAN_SAFE_BEGIN(op) \
    do { \
        PyThreadState *_tstate = PyThreadState_GET(); \
        if (_tstate->trash_delete_nesting < PyTrash_UNWIND_LEVEL) { \
            ++_tstate->trash_delete_nesting;
            /* The body of the deallocator is here. */
#define Py_TRASHCAN_SAFE_END(op) \
            --_tstate->trash_delete_nesting; \
            if (_tstate->trash_delete_later && _tstate->trash_delete_nesting <= 0) \
                _PyTrash_thread_destroy_chain(); \
        } \
        else \
            _PyTrash_thread_deposit_object((PyObject*)op); \
    } while (0);

#ifndef Py_LIMITED_API
PyAPI_FUNC(void)
_PyDebugAllocatorStats(FILE *out, const char *block_name, int num_blocks,
                       size_t sizeof_block);
PyAPI_FUNC(void)
_PyObject_DebugTypeStats(FILE *out);
#endif /* ifndef Py_LIMITED_API */

#ifdef __cplusplus
}
#endif
#endif /* !Py_OBJECT_H */
