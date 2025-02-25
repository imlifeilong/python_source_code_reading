#ifndef Py_INTERNAL_MEM_H
#define Py_INTERNAL_MEM_H
#ifdef __cplusplus
extern "C" {
#endif

#include "objimpl.h"
#include "pymem.h"


/* GC runtime state */

/* If we change this, we need to change the default value in the
   signature of gc.collect. */
#define NUM_GENERATIONS 3

/*
   NOTE: about the counting of long-lived objects.

   To limit the cost of garbage collection, there are two strategies;
     - make each collection faster, e.g. by scanning fewer objects
     - do less collections
   This heuristic is about the latter strategy.

   In addition to the various configurable thresholds, we only trigger a
   full collection if the ratio
    long_lived_pending / long_lived_total
   is above a given value (hardwired to 25%).

   The reason is that, while "non-full" collections (i.e., collections of
   the young and middle generations) will always examine roughly the same
   number of objects -- determined by the aforementioned thresholds --,
   the cost of a full collection is proportional to the total number of
   long-lived objects, which is virtually unbounded.

   Indeed, it has been remarked that doing a full collection every
   <constant number> of object creations entails a dramatic performance
   degradation in workloads which consist in creating and storing lots of
   long-lived objects (e.g. building a large list of GC-tracked objects would
   show quadratic performance, instead of linear as expected: see issue #4074).

   Using the above ratio, instead, yields amortized linear performance in
   the total number of objects (the effect of which can be summarized
   thusly: "each full garbage collection is more and more costly as the
   number of objects grows, but we do fewer and fewer of them").

   This heuristic was suggested by Martin von Löwis on python-dev in
   June 2008. His original analysis and proposal can be found at:
    http://mail.python.org/pipermail/python-dev/2008-June/080579.html
*/

/*
   NOTE: about untracking of mutable objects.

   Certain types of container cannot participate in a reference cycle, and
   so do not need to be tracked by the garbage collector. Untracking these
   objects reduces the cost of garbage collections. However, determining
   which objects may be untracked is not free, and the costs must be
   weighed against the benefits for garbage collection.

   There are two possible strategies for when to untrack a container:

   i) When the container is created.
   ii) When the container is examined by the garbage collector.

   Tuples containing only immutable objects (integers, strings etc, and
   recursively, tuples of immutable objects) do not need to be tracked.
   The interpreter creates a large number of tuples, many of which will
   not survive until garbage collection. It is therefore not worthwhile
   to untrack eligible tuples at creation time.

   Instead, all tuples except the empty tuple are tracked when created.
   During garbage collection it is determined whether any surviving tuples
   can be untracked. A tuple can be untracked if all of its contents are
   already not tracked. Tuples are examined for untracking in all garbage
   collection cycles. It may take more than one cycle to untrack a tuple.

   Dictionaries containing only immutable objects also do not need to be
   tracked. Dictionaries are untracked when created. If a tracked item is
   inserted into a dictionary (either as a key or value), the dictionary
   becomes tracked. During a full garbage collection (all generations),
   the collector will untrack any dictionaries whose contents are not
   tracked.

   The module provides the python function is_tracked(obj), which returns
   the CURRENT tracking status of the object. Subsequent garbage
   collections may change the tracking status of the object.

   Untracking of certain containers was introduced in issue #4688, and
   the algorithm was refined in response to issue #14775.
*/

// 定义一个名为 gc_generation 的结构体，该结构体用于表示 Python 垃圾回收机制中的一代。
// Python 的垃圾回收采用分代回收策略，将对象分为不同的代，每一代有不同的回收频率，
// 通常新创建的对象属于较年轻的代，随着对象存活时间增长，会逐渐晋升到较老的代。
struct gc_generation {
    // 每个代都有一个 PyGC_Head 类型的成员 head，它作为该代中所有可被垃圾回收对象的链表头。
    // PyGC_Head 结构体包含了用于构建双向链表的指针（gc_next 和 gc_prev）以及引用计数（gc_refs），
    // 通过这个链表，垃圾回收器可以遍历该代中的所有对象，以便进行引用计数检查和垃圾回收操作。
    PyGC_Head head;
    // threshold 是一个整数，代表该代的垃圾回收阈值。
    // 当该代中对象的数量（由 count 成员记录）超过这个阈值时，
    // 垃圾回收机制会触发对该代及更老代的对象进行垃圾回收操作。
    // 不同代的阈值通常不同，年轻代的阈值相对较低，意味着更频繁地进行回收，
    // 而老年代的阈值相对较高，回收频率较低。
    int threshold; /* collection threshold */
    // count 也是一个整数，用于记录该代中对象的分配数量或者更年轻代的垃圾回收次数。
    // 对于较年轻的代，它主要记录新分配的可被垃圾回收对象的数量；
    // 对于较老的代，当较年轻的代触发垃圾回收时，该代的 count 值也会相应增加。
    // 当 count 值超过 threshold 时，就会触发该代及更老代的垃圾回收。
    int count; /* count of allocations or collections of younger
                    generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    /* total number of collections */
    Py_ssize_t collections;
    /* total number of collected objects */
    Py_ssize_t collected;
    /* total number of uncollectable objects (put into gc.garbage) */
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject *trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int trash_delete_nesting;

    int enabled;
    int debug;
    /* linked lists of container objects */
    struct gc_generation generations[NUM_GENERATIONS];
    PyGC_Head *generation0;
    /* a permanent generation which won't be collected */
    struct gc_generation permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t long_lived_pending;
};

PyAPI_FUNC(void) _PyGC_Initialize(struct _gc_runtime_state *);

#define _PyGC_generation0 _PyRuntime.gc.generation0

/* Heuristic checking if a pointer value is newly allocated
   (uninitialized) or newly freed. The pointer is not dereferenced, only the
   pointer value is checked.

   The heuristic relies on the debug hooks on Python memory allocators which
   fills newly allocated memory with CLEANBYTE (0xCD) and newly freed memory
   with DEADBYTE (0xDD). Detect also "untouchable bytes" marked
   with FORBIDDENBYTE (0xFD). */
static inline int _PyMem_IsPtrFreed(void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
#if SIZEOF_VOID_P == 8
    return (value == (uintptr_t)0xCDCDCDCDCDCDCDCD
            || value == (uintptr_t)0xDDDDDDDDDDDDDDDD
            || value == (uintptr_t)0xFDFDFDFDFDFDFDFD);
#elif SIZEOF_VOID_P == 4
    return (value == (uintptr_t)0xCDCDCDCD
            || value == (uintptr_t)0xDDDDDDDD
            || value == (uintptr_t)0xFDFDFDFD);
#else
#  error "unknown pointer size"
#endif
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_MEM_H */
