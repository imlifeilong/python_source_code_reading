/*

  Reference Cycle Garbage Collection
  ==================================

  Neil Schemenauer <nas@arctrix.com>

  Based on a post on the python-dev list.  Ideas from Guido van Rossum,
  Eric Tiedemann, and various others.

  http://www.arctrix.com/nas/python/gc/

  The following mailing list threads provide a historical perspective on
  the design of this module.  Note that a fair amount of refinement has
  occurred since those discussions.

  http://mail.python.org/pipermail/python-dev/2000-March/002385.html
  http://mail.python.org/pipermail/python-dev/2000-March/002434.html
  http://mail.python.org/pipermail/python-dev/2000-March/002497.html

  For a highlevel view of the collection process, read the collect
  function.

*/

#include "Python.h"
#include "internal/context.h"
#include "internal/mem.h"
#include "internal/pystate.h"
#include "frameobject.h"        /* for PyFrame_ClearFreeList */
#include "pydtrace.h"
#include "pytime.h"             /* for _PyTime_GetMonotonicClock() */

/*[clinic input]
module gc
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=b5c9690ecc842d79]*/

/* Get an object's GC head */
/*
该宏的作用是从一个 Python 对象指针（PyObject *）获取其对应的垃圾回收头部指针（PyGC_Head *）。

在 Python 的内存布局中，每个支持垃圾回收的对象在其实际数据存储区域之前会有一个 PyGC_Head 结构体，
用于存储垃圾回收相关的信息，如引用计数、双向链表指针等。因此，要获取对象的垃圾回收头部，
只需要将对象指针向前移动 PyGC_Head 结构体大小的距离。

(PyGC_Head *)(o)：首先将对象指针 o 强制转换为 PyGC_Head * 类型，
这样做是为了后续指针运算能够按照 PyGC_Head 结构体的大小进行。

((PyGC_Head *)(o)-1)：将转换后的指针减去 1。在指针运算中，减去 1 
意味着指针向前移动一个 PyGC_Head 结构体大小的距离，从而得到该对象的垃圾回收头部指针。
*/
#define AS_GC(o) ((PyGC_Head *)(o)-1)

/* Get the object given the GC head */
/*
该宏的作用是从一个垃圾回收头部指针（PyGC_Head *）获取其对应的 Python 对象指针（PyObject *）。

由于 PyGC_Head 结构体位于 Python 对象实际数据存储区域之前，所以要从垃圾回收头部指针获取对象指针，
只需要将垃圾回收头部指针向后移动 PyGC_Head 结构体大小的距离。

(PyGC_Head *)g：将传入的指针 g 强制转换为 PyGC_Head * 类型，
确保后续指针运算按照 PyGC_Head 结构体的大小进行。

((PyGC_Head *)g)+1：将转换后的指针加上 1。在指针运算中，加上 1 
意味着指针向后移动一个 PyGC_Head 结构体大小的距离，从而得到该垃圾回收头部对应的 Python 对象指针。

(PyObject *)(((PyGC_Head *)g)+1)：最后将得到的指针强制转换为 PyObject * 类型，以便返回一个 Python 对象指针。
*/
#define FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))

/* Python string to use if unhandled exception occurs */
static PyObject *gc_str = NULL;

/* set for debugging information */
#define DEBUG_STATS             (1<<0) /* print collection statistics */
#define DEBUG_COLLECTABLE       (1<<1) /* print collectable objects */
#define DEBUG_UNCOLLECTABLE     (1<<2) /* print uncollectable objects */
#define DEBUG_SAVEALL           (1<<5) /* save all garbage in gc.garbage */
#define DEBUG_LEAK              DEBUG_COLLECTABLE | \
                DEBUG_UNCOLLECTABLE | \
                DEBUG_SAVEALL

#define GEN_HEAD(n) (&_PyRuntime.gc.generations[n].head)

/*
作用：初始化垃圾回收器的运行时状态。
参数：state 指向 GC 运行时状态的结构体，用于存储所有 GC 相关配置和数据。
*/
void
_PyGC_Initialize(struct _gc_runtime_state *state)
{
    // 1 表示启用自动垃圾回收
    state->enabled = 1; /* automatic collection enabled? */

    // 定义一个宏 _GEN_HEAD(n)，用于获取指定代数（n）的垃圾回收链表头指针
    // 该宏通过访问 state 结构体中 generations 数组的第 n 个元素的 head 成员来实现
#define _GEN_HEAD(n) (&state->generations[n].head)
    // 定义一个局部数组 generations，用于初始化不同代数的垃圾回收信息
    // 数组的大小由 NUM_GENERATIONS 宏定义，代表垃圾回收的代数数量
    struct gc_generation generations[NUM_GENERATIONS] = {
        // 每一代垃圾回收信息包含一个 PyGC_Head 结构体、一个阈值和一个计数
        // PyGC_Head 结构体用于构建双向链表，阈值用于决定何时触发垃圾回收，计数用于记录当前代数中对象的数量
        /* PyGC_Head,                                 threshold,      count */
        // 第 0 代垃圾回收信息初始化
        // 双向链表的头指针和尾指针都指向自身，阈值为 700，初始计数为 0
        {{{_GEN_HEAD(0), _GEN_HEAD(0), 0}},           700,            0},
        // 第 1 代垃圾回收信息初始化
        // 双向链表的头指针和尾指针都指向自身，阈值为 10，初始计数为 0
        {{{_GEN_HEAD(1), _GEN_HEAD(1), 0}},           10,             0},
        // 第 2 代垃圾回收信息初始化
        // 双向链表的头指针和尾指针都指向自身，阈值为 10，初始计数为 0
        {{{_GEN_HEAD(2), _GEN_HEAD(2), 0}},           10,             0},
    };
    // 遍历 generations 数组，将局部数组中的信息复制到 state 结构体的 generations 数组中
    for (int i = 0; i < NUM_GENERATIONS; i++) {
        state->generations[i] = generations[i];
    };
    // 将第 0 代垃圾回收链表的头指针赋值给 state 结构体的 generation0 成员
    // 方便后续快速访问第 0 代垃圾回收链表
    state->generation0 = GEN_HEAD(0);
    // 定义一个永久代的垃圾回收信息结构体 permanent_generation
    // 永久代的双向链表的头指针和尾指针都指向自身，阈值为 0，初始计数为 0
    struct gc_generation permanent_generation = {
          {{&state->permanent_generation.head, &state->permanent_generation.head, 0}}, 0, 0
    };
    // 将永久代的垃圾回收信息赋值给 state 结构体的 permanent_generation 成员
    state->permanent_generation = permanent_generation;
}

/*--------------------------------------------------------------------------
gc_refs values.

Between collections, every gc'ed object has one of two gc_refs values:

GC_UNTRACKED
    The initial state; objects returned by PyObject_GC_Malloc are in this
    state.  The object doesn't live in any generation list, and its
    tp_traverse slot must not be called.

GC_REACHABLE
    The object lives in some generation list, and its tp_traverse is safe to
    call.  An object transitions to GC_REACHABLE when PyObject_GC_Track
    is called.

During a collection, gc_refs can temporarily take on other states:

>= 0
    At the start of a collection, update_refs() copies the true refcount
    to gc_refs, for each object in the generation being collected.
    subtract_refs() then adjusts gc_refs so that it equals the number of
    times an object is referenced directly from outside the generation
    being collected.
    gc_refs remains >= 0 throughout these steps.

GC_TENTATIVELY_UNREACHABLE
    move_unreachable() then moves objects not reachable (whether directly or
    indirectly) from outside the generation into an "unreachable" set.
    Objects that are found to be reachable have gc_refs set to GC_REACHABLE
    again.  Objects that are found to be unreachable have gc_refs set to
    GC_TENTATIVELY_UNREACHABLE.  It's "tentatively" because the pass doing
    this can't be sure until it ends, and GC_TENTATIVELY_UNREACHABLE may
    transition back to GC_REACHABLE.

    Only objects with GC_TENTATIVELY_UNREACHABLE still set are candidates
    for collection.  If it's decided not to collect such an object (e.g.,
    it has a __del__ method), its gc_refs is restored to GC_REACHABLE again.
----------------------------------------------------------------------------
*/
#define GC_UNTRACKED                    _PyGC_REFS_UNTRACKED
#define GC_REACHABLE                    _PyGC_REFS_REACHABLE
#define GC_TENTATIVELY_UNREACHABLE      _PyGC_REFS_TENTATIVELY_UNREACHABLE

#define IS_TRACKED(o) (_PyGC_REFS(o) != GC_UNTRACKED)
#define IS_REACHABLE(o) (_PyGC_REFS(o) == GC_REACHABLE)
#define IS_TENTATIVELY_UNREACHABLE(o) ( \
    _PyGC_REFS(o) == GC_TENTATIVELY_UNREACHABLE)

/*** list functions ***/

/**
 * gc_list_init 函数用于初始化一个基于 PyGC_Head 结构体的双向链表。
 * 在 Python 的垃圾回收机制中，会使用双向链表来管理可被垃圾回收的对象，
 * 该函数的作用是将给定的链表初始化为空链表状态。
 *
 * @param list 指向 PyGC_Head 结构体的指针，代表要初始化的链表头节点。
 */
static void
gc_list_init(PyGC_Head* list)
{
    // 将链表头节点的 gc_prev 指针指向自身。
    // 在双向链表中，gc_prev 指针通常用于指向前一个节点，
    // 当链表为空时，头节点的前一个节点就是它自己。
    list->gc.gc_prev = list;

    // 将链表头节点的 gc_next 指针指向自身。
    // 同理，gc_next 指针用于指向后一个节点，
    // 空链表的头节点的后一个节点也是它自己。
    list->gc.gc_next = list;
}
/**
 * gc_list_is_empty 函数用于判断一个基于 PyGC_Head 结构体构建的双向链表是否为空。
 * 在 Python 的垃圾回收机制中，会使用双向链表来管理可被垃圾回收的对象，
 * 该函数为判断链表是否为空提供了一种简单的方式。
 *
 * @param list 指向 PyGC_Head 结构体的指针，代表要检查的链表的头节点。
 * @return 如果链表为空，返回非零值（通常为 1）；如果链表不为空，返回 0。
 */
static int
gc_list_is_empty(PyGC_Head* list)
{
    // 在双向链表中，如果链表为空，头节点的 gc_next 指针会指向自身。
    // 因为在空链表中，没有其他节点，所以头节点的下一个节点就是它自己。
    // 这里通过比较头节点的 gc_next 指针和头节点本身是否相等来判断链表是否为空。
    // 如果相等，说明链表为空，返回非零值；否则，说明链表不为空，返回 0。
    return (list->gc.gc_next == list);
}

#if 0
/* This became unused after gc_list_move() was introduced. */
/* Append `node` to `list`. */
static void
gc_list_append(PyGC_Head *node, PyGC_Head *list)
{
    node->gc.gc_next = list;
    node->gc.gc_prev = list->gc.gc_prev;
    node->gc.gc_prev->gc.gc_next = node;
    list->gc.gc_prev = node;
}
#endif

/* Remove `node` from the gc list it's currently in. */
static void
gc_list_remove(PyGC_Head *node)
{
    node->gc.gc_prev->gc.gc_next = node->gc.gc_next;
    node->gc.gc_next->gc.gc_prev = node->gc.gc_prev;
    node->gc.gc_next = NULL; /* object is not currently tracked */
}

/* Move `node` from the gc list it's currently in (which is not explicitly
 * named here) to the end of `list`.  This is semantically the same as
 * gc_list_remove(node) followed by gc_list_append(node, list).
 */
static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    PyGC_Head *new_prev;
    PyGC_Head *current_prev = node->gc.gc_prev;
    PyGC_Head *current_next = node->gc.gc_next;
    /* Unlink from current list. */
    current_prev->gc.gc_next = current_next;
    current_next->gc.gc_prev = current_prev;
    /* Relink at end of new list. */
    new_prev = node->gc.gc_prev = list->gc.gc_prev;
    new_prev->gc.gc_next = list->gc.gc_prev = node;
    node->gc.gc_next = list;
}

/* append list `from` onto list `to`; `from` becomes an empty list */
/**
 * gc_list_merge 函数用于将一个垃圾回收对象链表（`from`）合并到另一个链表（`to`）中。
 * 这两个链表都是基于 `PyGC_Head` 结构体构建的双向链表，常用于 Python 的垃圾回收机制。
 *
 * @param from 指向要合并的源链表的 `PyGC_Head` 结构体指针。合并完成后，该链表会被清空。
 * @param to 指向目标链表的 `PyGC_Head` 结构体指针，源链表的元素将被添加到这个链表中。
 */
static void
gc_list_merge(PyGC_Head* from, PyGC_Head* to)
{
    // 定义一个指向 `PyGC_Head` 结构体的指针 `tail`，用于临时存储目标链表的尾部节点。
    PyGC_Head* tail;

    // 使用 `assert` 宏确保 `from` 和 `to` 不是同一个链表。
    // 如果 `from` 和 `to` 相同，合并操作没有意义，并且可能导致链表结构损坏，因此这里进行断言检查。
    assert(from != to);

    // 检查源链表 `from` 是否为空。
    // `gc_list_is_empty` 函数用于判断链表是否为空，如果链表不为空，则执行合并操作。
    if (!gc_list_is_empty(from)) {
        // 获取目标链表 `to` 的尾部节点，将其赋值给 `tail` 指针。
        tail = to->gc.gc_prev;

        // 以下四行代码完成链表节点的连接操作，将源链表 `from` 插入到目标链表 `to` 的尾部。

        // 将目标链表尾部节点 `tail` 的 `gc_next` 指针指向源链表的第一个有效节点（`from->gc.gc_next`）。
        tail->gc.gc_next = from->gc.gc_next;
        // 将源链表第一个有效节点的 `gc_prev` 指针指向目标链表的尾部节点 `tail`。
        tail->gc.gc_next->gc.gc_prev = tail;
        // 将目标链表 `to` 的 `gc_prev` 指针指向源链表的最后一个有效节点（`from->gc.gc_prev`）。
        to->gc.gc_prev = from->gc.gc_prev;
        // 将源链表最后一个有效节点的 `gc_next` 指针指向目标链表 `to`。
        to->gc.gc_prev->gc.gc_next = to;
    }

    // 合并完成后，将源链表 `from` 初始化为空链表。
    // `gc_list_init` 函数用于将链表初始化为空状态，通常会将链表的头节点和尾节点指向自身。
    gc_list_init(from);
}

static Py_ssize_t
gc_list_size(PyGC_Head *list)
{
    PyGC_Head *gc;
    Py_ssize_t n = 0;
    for (gc = list->gc.gc_next; gc != list; gc = gc->gc.gc_next) {
        n++;
    }
    return n;
}

/* Append objects in a GC list to a Python list.
 * Return 0 if all OK, < 0 if error (out of memory for list).
 */
static int
append_objects(PyObject *py_list, PyGC_Head *gc_list)
{
    PyGC_Head *gc;
    for (gc = gc_list->gc.gc_next; gc != gc_list; gc = gc->gc.gc_next) {
        PyObject *op = FROM_GC(gc);
        if (op != py_list) {
            if (PyList_Append(py_list, op)) {
                return -1; /* exception */
            }
        }
    }
    return 0;
}

/*** end of list stuff ***/


/* Set all gc_refs = ob_refcnt.  After this, gc_refs is > 0 for all objects
 * in containers, and is GC_REACHABLE for all tracked gc objects not in
 * containers.
 */
 // 函数功能说明：将所有对象的 gc_refs 字段设置为其引用计数 ob_refcnt。
 // 操作完成后，所有容器内对象的 gc_refs 大于 0，所有未在容器内且被垃圾回收机制跟踪的对象的 gc_refs 为 GC_REACHABLE
static void
update_refs(PyGC_Head* containers)
{
    // 从 containers 指向的下一个对象开始遍历
    PyGC_Head* gc = containers->gc.gc_next;
    // 循环遍历所有被垃圾回收机制跟踪的对象，直到回到 containers 本身
    for (; gc != containers; gc = gc->gc.gc_next) {
        // 断言当前对象的 gc_refs 为 GC_REACHABLE
        // 这是为了确保在更新之前，对象的状态是符合预期的
        assert(_PyGCHead_REFS(gc) == GC_REACHABLE);
        // 将当前对象的 gc_refs 设置为其引用计数
        // FROM_GC(gc) 用于从 PyGC_Head 指针获取对应的 Python 对象指针
        // Py_REFCNT 用于获取该对象的引用计数
        _PyGCHead_SET_REFS(gc, Py_REFCNT(FROM_GC(gc)));
        /* Python's cyclic gc should never see an incoming refcount
         * of 0:  if something decref'ed to 0, it should have been
         * deallocated immediately at that time.
         * Possible cause (if the assert triggers):  a tp_dealloc
         * routine left a gc-aware object tracked during its teardown
         * phase, and did something-- or allowed something to happen --
         * that called back into Python.  gc can trigger then, and may
         * see the still-tracked dying object.  Before this assert
         * was added, such mistakes went on to allow gc to try to
         * delete the object again.  In a debug build, that caused
         * a mysterious segfault, when _Py_ForgetReference tried
         * to remove the object from the doubly-linked list of all
         * objects a second time.  In a release build, an actual
         * double deallocation occurred, which leads to corruption
         * of the allocator's internal bookkeeping pointers.  That's
         * so serious that maybe this should be a release-build
         * check instead of an assert?
         */
         // 注释说明：Python 的循环垃圾回收机制不应该看到引用计数为 0 的对象。
         // 如果一个对象的引用计数减为 0，它应该立即被释放。
         // 如果断言触发，可能的原因是：在对象的析构函数（tp_dealloc）中，一个支持垃圾回收的对象
         // 在其销毁阶段仍被跟踪，并且做了一些操作或者允许一些操作回调到 Python 中，
         // 这时垃圾回收机制可能会触发，并且看到仍在被跟踪的即将销毁的对象。
         // 在添加这个断言之前，这样的错误会导致垃圾回收机制尝试再次删除该对象。
         // 在调试版本中，当 _Py_ForgetReference 尝试第二次从所有对象的双向链表中移除该对象时，
         // 会导致神秘的段错误。在发布版本中，会发生实际的双重释放，这会导致内存分配器的内部记账指针损坏。
         // 这种情况非常严重，也许这个检查应该在发布版本中进行，而不仅仅是作为一个断言。
        assert(_PyGCHead_REFS(gc) != 0);
    }
}

/* A traversal callback for subtract_refs. */
// 声明这是一个用于 subtract_refs 的遍历回调函数。在 Python 的垃圾回收机制中，
// 经常会使用回调函数来遍历对象图，对每个对象执行特定的操作，这里的操作就是减少引用计数。
static int
visit_decref(PyObject* op, void* data)
{
    // 断言 op 不为空指针。这是一种防御性编程的手段，确保传入的对象指针是有效的。
    assert(op != NULL);
    // 检查对象 op 是否是支持垃圾回收的对象。在 Python 中，只有部分对象类型支持垃圾回收，
    // 这些对象会包含一个 PyGC_Head 结构来管理其垃圾回收相关的信息。
    if (PyObject_IS_GC(op)) {
        // 如果对象支持垃圾回收，将对象指针转换为 PyGC_Head 指针。
        // PyGC_Head 是 Python 中用于管理垃圾回收的头部结构，包含了对象的引用计数等信息。
        PyGC_Head* gc = AS_GC(op);
        /* We're only interested in gc_refs for objects in the
         * generation being collected, which can be recognized
         * because only they have positive gc_refs.
         */
         // 注释解释了我们只关注正在被回收的代（generation）中的对象的 gc_refs。
         // 在 Python 的分代垃圾回收机制中，对象被分为不同的代，每一代有不同的回收频率。
         // 只有正在被回收的代中的对象的 gc_refs 是正数，其他对象的 gc_refs 可能为负数或 0。
         // 断言 gc_refs 不为 0，如果为 0 说明对象的引用计数过小，可能存在错误。
        assert(_PyGCHead_REFS(gc) != 0); /* else refcount was too small */
        // 检查对象的 gc_refs 是否为正数。如果为正数，说明该对象是正在被回收的代中的对象。
        if (_PyGCHead_REFS(gc) > 0)
            // 如果对象的 gc_refs 为正数，调用 _PyGCHead_DECREF 函数将其引用计数减 1。
            // 这是垃圾回收过程中的一个重要步骤，通过减少引用计数来标记对象是否可以被回收。
            _PyGCHead_DECREF(gc);
    }
    // 回调函数返回 0 表示继续遍历。在 Python 的遍历机制中，回调函数返回 0 表示正常处理，
    // 可以继续遍历下一个对象；返回非 0 值表示出现错误或需要停止遍历。
    return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
 // 定义一个静态函数 subtract_refs，它接受一个指向 PyGC_Head 结构体的指针 containers 作为参数
 // 这个函数用于减去对象之间的循环引用计数
static void
subtract_refs(PyGC_Head* containers)
{
    // 声明一个 traverseproc 类型的变量 traverse
    // traverseproc 是一个函数指针类型，用于指向对象的 tp_traverse 方法
    traverseproc traverse;
    // 从 containers 指向的下一个对象开始遍历
    // 因为 containers 本身是一个哨兵节点，真正需要处理的对象从它的下一个节点开始
    PyGC_Head* gc = containers->gc.gc_next;
    // 循环遍历所有被垃圾回收机制跟踪的对象，直到回到 containers 本身
    for (; gc != containers; gc = gc->gc.gc_next) {
        // 获取当前对象的类型对象，并从中提取 tp_traverse 方法
        // Py_TYPE(FROM_GC(gc)) 用于获取当前 PyGC_Head 对应的 Python 对象的类型对象
        // tp_traverse 是类型对象中的一个函数指针，用于遍历对象的引用
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        // 调用当前对象的 tp_traverse 方法
        // FROM_GC(gc) 是当前对象的指针
        // (visitproc)visit_decref 是一个访问函数，用于在遍历过程中减去引用计数
        // NULL 是传递给访问函数的额外参数，这里没有使用
        // (void) 强制类型转换是为了避免编译器发出未使用返回值的警告
        (void)traverse(FROM_GC(gc),
            (visitproc)visit_decref,
            NULL);
    }
}

/* A traversal callback for move_unreachable. */
static int
visit_reachable(PyObject *op, PyGC_Head *reachable)
{
    if (PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        const Py_ssize_t gc_refs = _PyGCHead_REFS(gc);

        if (gc_refs == 0) {
            /* This is in move_unreachable's 'young' list, but
             * the traversal hasn't yet gotten to it.  All
             * we need to do is tell move_unreachable that it's
             * reachable.
             */
            _PyGCHead_SET_REFS(gc, 1);
        }
        else if (gc_refs == GC_TENTATIVELY_UNREACHABLE) {
            /* This had gc_refs = 0 when move_unreachable got
             * to it, but turns out it's reachable after all.
             * Move it back to move_unreachable's 'young' list,
             * and move_unreachable will eventually get to it
             * again.
             */
            gc_list_move(gc, reachable);
            _PyGCHead_SET_REFS(gc, 1);
        }
        /* Else there's nothing to do.
         * If gc_refs > 0, it must be in move_unreachable's 'young'
         * list, and move_unreachable will eventually get to it.
         * If gc_refs == GC_REACHABLE, it's either in some other
         * generation so we don't care about it, or move_unreachable
         * already dealt with it.
         * If gc_refs == GC_UNTRACKED, it must be ignored.
         */
         else {
            assert(gc_refs > 0
                   || gc_refs == GC_REACHABLE
                   || gc_refs == GC_UNTRACKED);
         }
    }
    return 0;
}

/* Move the unreachable objects from young to unreachable.  After this,
 * all objects in young have gc_refs = GC_REACHABLE, and all objects in
 * unreachable have gc_refs = GC_TENTATIVELY_UNREACHABLE.  All tracked
 * gc objects not in young or unreachable still have gc_refs = GC_REACHABLE.
 * All objects in young after this are directly or indirectly reachable
 * from outside the original young; and all objects in unreachable are
 * not.
 */
 // 定义一个静态函数 move_unreachable，它接受两个指向 PyGC_Head 结构体的指针 young 和 unreachable 作为参数
 // young 链表包含了需要进行垃圾回收检查的对象，unreachable 链表用于存放可能不可达的对象
static void
move_unreachable(PyGC_Head* young, PyGC_Head* unreachable)
{
    // 从 young 链表的第一个对象开始遍历
    PyGC_Head* gc = young->gc.gc_next;

    /* Invariants:  all objects "to the left" of us in young have gc_refs
     * = GC_REACHABLE, and are indeed reachable (directly or indirectly)
     * from outside the young list as it was at entry.  All other objects
     * from the original young "to the left" of us are in unreachable now,
     * and have gc_refs = GC_TENTATIVELY_UNREACHABLE.  All objects to the
     * left of us in 'young' now have been scanned, and no objects here
     * or to the right have been scanned yet.
     */
     // 不变式说明：
     // 在 young 链表中，当前遍历位置“左侧”的所有对象的 gc_refs 为 GC_REACHABLE，
     // 并且这些对象确实从进入函数时 young 链表外部（直接或间接）可达。
     // 原 young 链表中当前遍历位置“左侧”的其他对象现在都在 unreachable 链表中，
     // 并且它们的 gc_refs 为 GC_TENTATIVELY_UNREACHABLE。
     // young 链表中当前遍历位置“左侧”的所有对象都已经被扫描过，而当前位置及其“右侧”的对象还未被扫描。

     // 开始遍历 young 链表，直到回到链表头（young）
    while (gc != young) {
        // 保存当前对象的下一个对象指针，因为在处理过程中当前对象的位置可能会改变
        PyGC_Head* next;

        // 检查当前对象的 gc_refs 是否不为 0
        if (_PyGCHead_REFS(gc)) {
            /* gc is definitely reachable from outside the
             * original 'young'.  Mark it as such, and traverse
             * its pointers to find any other objects that may
             * be directly reachable from it.  Note that the
             * call to tp_traverse may append objects to young,
             * so we have to wait until it returns to determine
             * the next object to visit.
             */
             // 说明：当前对象肯定从原 young 链表外部可达。
             // 标记它为可达，并遍历它的引用，以找出其他可能直接从它可达的对象。
             // 注意：调用 tp_traverse 可能会向 young 链表追加对象，所以我们必须等它返回后再确定下一个要访问的对象。
             // 获取当前 PyGC_Head 对应的 Python 对象指针
            PyObject* op = FROM_GC(gc);
            // 获取当前对象类型的 tp_traverse 方法，用于遍历对象的引用
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            // 断言当前对象的 gc_refs 大于 0，确保对象可达
            assert(_PyGCHead_REFS(gc) > 0);
            // 将当前对象的 gc_refs 标记为 GC_REACHABLE，表示它是可达的
            _PyGCHead_SET_REFS(gc, GC_REACHABLE);
            // 调用 tp_traverse 方法，传入 visit_reachable 作为访问函数，young 作为额外参数
            // 该方法会遍历当前对象的引用，并将可达对象标记为可达
            (void)traverse(op,
                (visitproc)visit_reachable,
                (void*)young);
            // 获取当前对象的下一个对象指针
            next = gc->gc.gc_next;
            // 如果当前对象是元组类型，尝试取消对它的跟踪
            if (PyTuple_CheckExact(op)) {
                _PyTuple_MaybeUntrack(op);
            }
        }
        else {
            /* This *may* be unreachable.  To make progress,
             * assume it is.  gc isn't directly reachable from
             * any object we've already traversed, but may be
             * reachable from an object we haven't gotten to yet.
             * visit_reachable will eventually move gc back into
             * young if that's so, and we'll see it again.
             */
             // 说明：当前对象可能不可达。为了继续处理，假设它不可达。
             // 当前对象不能从我们已经遍历过的任何对象直接可达，但可能从我们还未处理的对象可达。
             // 如果是这样，visit_reachable 最终会将当前对象移回 young 链表，我们会再次处理它。
             // 获取当前对象的下一个对象指针
            next = gc->gc.gc_next;
            // 将当前对象从 young 链表移动到 unreachable 链表
            gc_list_move(gc, unreachable);
            // 将当前对象的 gc_refs 标记为 GC_TENTATIVELY_UNREACHABLE，表示它可能不可达
            _PyGCHead_SET_REFS(gc, GC_TENTATIVELY_UNREACHABLE);
        }
        // 移动到下一个对象继续遍历
        gc = next;
    }
}
/* Try to untrack all currently tracked dictionaries */
static void
untrack_dicts(PyGC_Head *head)
{
    PyGC_Head *next, *gc = head->gc.gc_next;
    while (gc != head) {
        PyObject *op = FROM_GC(gc);
        next = gc->gc.gc_next;
        if (PyDict_CheckExact(op))
            _PyDict_MaybeUntrack(op);
        gc = next;
    }
}

/* Return true if object has a pre-PEP 442 finalization method. */
static int
has_legacy_finalizer(PyObject *op)
{
    return op->ob_type->tp_del != NULL;
}

/* Move the objects in unreachable with tp_del slots into `finalizers`.
 * Objects moved into `finalizers` have gc_refs set to GC_REACHABLE; the
 * objects remaining in unreachable are left at GC_TENTATIVELY_UNREACHABLE.
 */
 // 定义一个静态函数 move_legacy_finalizers，它接受两个指向 PyGC_Head 结构体的指针
 // unreachable 链表包含了之前被标记为可能不可达的对象
 // finalizers 链表用于存放具有旧版终结器的对象
static void
move_legacy_finalizers(PyGC_Head* unreachable, PyGC_Head* finalizers)
{
    // 定义两个指向 PyGC_Head 结构体的指针
    // gc 用于遍历 unreachable 链表中的对象
    // next 用于保存当前对象的下一个对象，因为在移动对象时当前对象的链表关系会改变
    PyGC_Head* gc;
    PyGC_Head* next;

    /* March over unreachable.  Move objects with finalizers into
     * `finalizers`.
     */
     // 说明：遍历 unreachable 链表，将具有终结器的对象移动到 finalizers 链表中

     // 从 unreachable 链表的第一个对象开始遍历，直到回到链表头（unreachable）
    for (gc = unreachable->gc.gc_next; gc != unreachable; gc = next) {
        // 获取当前 PyGC_Head 对应的 Python 对象指针
        PyObject* op = FROM_GC(gc);

        // 断言当前对象处于可能不可达状态
        // 确保当前对象确实是之前标记为可能不可达的对象
        assert(IS_TENTATIVELY_UNREACHABLE(op));

        // 保存当前对象的下一个对象指针
        // 因为后续可能会将当前对象从 unreachable 链表中移除，所以需要提前保存下一个对象
        next = gc->gc.gc_next;

        // 检查当前对象是否具有旧版终结器
        if (has_legacy_finalizer(op)) {
            // 如果具有旧版终结器，将当前对象从 unreachable 链表移动到 finalizers 链表
            gc_list_move(gc, finalizers);
            // 将该对象的引用计数状态标记为可达
            // 因为要对这些有终结器的对象进行单独处理，暂时认为它们是可达的
            _PyGCHead_SET_REFS(gc, GC_REACHABLE);
        }
    }
}
/* A traversal callback for move_legacy_finalizer_reachable. */
static int
visit_move(PyObject *op, PyGC_Head *tolist)
{
    if (PyObject_IS_GC(op)) {
        if (IS_TENTATIVELY_UNREACHABLE(op)) {
            PyGC_Head *gc = AS_GC(op);
            gc_list_move(gc, tolist);
            _PyGCHead_SET_REFS(gc, GC_REACHABLE);
        }
    }
    return 0;
}

/* Move objects that are reachable from finalizers, from the unreachable set
 * into finalizers set.
 */
 // 定义一个静态函数 move_legacy_finalizer_reachable，它接受一个指向 PyGC_Head 结构体的指针 finalizers 作为参数
 // finalizers 链表存放着具有旧版终结器的对象
static void
move_legacy_finalizer_reachable(PyGC_Head* finalizers)
{
    // 声明一个 traverseproc 类型的变量 traverse
    // traverseproc 是一个函数指针类型，用于指向对象的 tp_traverse 方法，该方法用于遍历对象的引用
    traverseproc traverse;
    // 从 finalizers 链表的第一个对象开始遍历
    // 因为 finalizers 本身可看作链表的头节点，真正要处理的对象从它的下一个节点开始
    PyGC_Head* gc = finalizers->gc.gc_next;
    // 循环遍历 finalizers 链表中的所有对象，直到回到链表头（finalizers）
    for (; gc != finalizers; gc = gc->gc.gc_next) {
        /* Note that the finalizers list may grow during this. */
        // 注释说明：在遍历过程中，finalizers 链表可能会增长
        // 这是因为在调用 tp_traverse 方法并使用 visit_move 访问函数时，
        // 可能会发现新的可达对象并将它们添加到 finalizers 链表中

        // 获取当前对象的类型对象，并从中提取 tp_traverse 方法
        // Py_TYPE(FROM_GC(gc)) 用于获取当前 PyGC_Head 对应的 Python 对象的类型对象
        // tp_traverse 是类型对象中的一个函数指针，用于遍历对象的引用
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        // 调用当前对象的 tp_traverse 方法
        // FROM_GC(gc) 是当前对象的指针
        // (visitproc)visit_move 是一个访问函数，用于在遍历过程中处理可达对象，可能会将可达对象移动到 finalizers 链表
        // (void *)finalizers 是传递给访问函数的额外参数，这里传递 finalizers 链表指针，方便访问函数操作
        // (void) 强制类型转换是为了避免编译器发出未使用返回值的警告
        (void)traverse(FROM_GC(gc),
            (visitproc)visit_move,
            (void*)finalizers);
    }
}

/* Clear all weakrefs to unreachable objects, and if such a weakref has a
 * callback, invoke it if necessary.  Note that it's possible for such
 * weakrefs to be outside the unreachable set -- indeed, those are precisely
 * the weakrefs whose callbacks must be invoked.  See gc_weakref.txt for
 * overview & some details.  Some weakrefs with callbacks may be reclaimed
 * directly by this routine; the number reclaimed is the return value.  Other
 * weakrefs with callbacks may be moved into the `old` generation.  Objects
 * moved into `old` have gc_refs set to GC_REACHABLE; the objects remaining in
 * unreachable are left at GC_TENTATIVELY_UNREACHABLE.  When this returns,
 * no object in `unreachable` is weakly referenced anymore.
 */
static int
handle_weakrefs(PyGC_Head *unreachable, PyGC_Head *old)
{
    PyGC_Head *gc;
    PyObject *op;               /* generally FROM_GC(gc) */
    PyWeakReference *wr;        /* generally a cast of op */
    PyGC_Head wrcb_to_call;     /* weakrefs with callbacks to call */
    PyGC_Head *next;
    int num_freed = 0;

    gc_list_init(&wrcb_to_call);

    /* Clear all weakrefs to the objects in unreachable.  If such a weakref
     * also has a callback, move it into `wrcb_to_call` if the callback
     * needs to be invoked.  Note that we cannot invoke any callbacks until
     * all weakrefs to unreachable objects are cleared, lest the callback
     * resurrect an unreachable object via a still-active weakref.  We
     * make another pass over wrcb_to_call, invoking callbacks, after this
     * pass completes.
     */
    for (gc = unreachable->gc.gc_next; gc != unreachable; gc = next) {
        PyWeakReference **wrlist;

        op = FROM_GC(gc);
        assert(IS_TENTATIVELY_UNREACHABLE(op));
        next = gc->gc.gc_next;

        if (! PyType_SUPPORTS_WEAKREFS(Py_TYPE(op)))
            continue;

        /* It supports weakrefs.  Does it have any? */
        wrlist = (PyWeakReference **)
                                PyObject_GET_WEAKREFS_LISTPTR(op);

        /* `op` may have some weakrefs.  March over the list, clear
         * all the weakrefs, and move the weakrefs with callbacks
         * that must be called into wrcb_to_call.
         */
        for (wr = *wrlist; wr != NULL; wr = *wrlist) {
            PyGC_Head *wrasgc;                  /* AS_GC(wr) */

            /* _PyWeakref_ClearRef clears the weakref but leaves
             * the callback pointer intact.  Obscure:  it also
             * changes *wrlist.
             */
            assert(wr->wr_object == op);
            _PyWeakref_ClearRef(wr);
            assert(wr->wr_object == Py_None);
            if (wr->wr_callback == NULL)
                continue;                       /* no callback */

    /* Headache time.  `op` is going away, and is weakly referenced by
     * `wr`, which has a callback.  Should the callback be invoked?  If wr
     * is also trash, no:
     *
     * 1. There's no need to call it.  The object and the weakref are
     *    both going away, so it's legitimate to pretend the weakref is
     *    going away first.  The user has to ensure a weakref outlives its
     *    referent if they want a guarantee that the wr callback will get
     *    invoked.
     *
     * 2. It may be catastrophic to call it.  If the callback is also in
     *    cyclic trash (CT), then although the CT is unreachable from
     *    outside the current generation, CT may be reachable from the
     *    callback.  Then the callback could resurrect insane objects.
     *
     * Since the callback is never needed and may be unsafe in this case,
     * wr is simply left in the unreachable set.  Note that because we
     * already called _PyWeakref_ClearRef(wr), its callback will never
     * trigger.
     *
     * OTOH, if wr isn't part of CT, we should invoke the callback:  the
     * weakref outlived the trash.  Note that since wr isn't CT in this
     * case, its callback can't be CT either -- wr acted as an external
     * root to this generation, and therefore its callback did too.  So
     * nothing in CT is reachable from the callback either, so it's hard
     * to imagine how calling it later could create a problem for us.  wr
     * is moved to wrcb_to_call in this case.
     */
            if (IS_TENTATIVELY_UNREACHABLE(wr))
                continue;
            assert(IS_REACHABLE(wr));

            /* Create a new reference so that wr can't go away
             * before we can process it again.
             */
            Py_INCREF(wr);

            /* Move wr to wrcb_to_call, for the next pass. */
            wrasgc = AS_GC(wr);
            assert(wrasgc != next); /* wrasgc is reachable, but
                                       next isn't, so they can't
                                       be the same */
            gc_list_move(wrasgc, &wrcb_to_call);
        }
    }

    /* Invoke the callbacks we decided to honor.  It's safe to invoke them
     * because they can't reference unreachable objects.
     */
    while (! gc_list_is_empty(&wrcb_to_call)) {
        PyObject *temp;
        PyObject *callback;

        gc = wrcb_to_call.gc.gc_next;
        op = FROM_GC(gc);
        assert(IS_REACHABLE(op));
        assert(PyWeakref_Check(op));
        wr = (PyWeakReference *)op;
        callback = wr->wr_callback;
        assert(callback != NULL);

        /* copy-paste of weakrefobject.c's handle_callback() */
        temp = PyObject_CallFunctionObjArgs(callback, wr, NULL);
        if (temp == NULL)
            PyErr_WriteUnraisable(callback);
        else
            Py_DECREF(temp);

        /* Give up the reference we created in the first pass.  When
         * op's refcount hits 0 (which it may or may not do right now),
         * op's tp_dealloc will decref op->wr_callback too.  Note
         * that the refcount probably will hit 0 now, and because this
         * weakref was reachable to begin with, gc didn't already
         * add it to its count of freed objects.  Example:  a reachable
         * weak value dict maps some key to this reachable weakref.
         * The callback removes this key->weakref mapping from the
         * dict, leaving no other references to the weakref (excepting
         * ours).
         */
        Py_DECREF(op);
        if (wrcb_to_call.gc.gc_next == gc) {
            /* object is still alive -- move it */
            gc_list_move(gc, old);
        }
        else
            ++num_freed;
    }

    return num_freed;
}

static void
debug_cycle(const char *msg, PyObject *op)
{
    PySys_FormatStderr("gc: %s <%s %p>\n",
                       msg, Py_TYPE(op)->tp_name, op);
}

/* Handle uncollectable garbage (cycles with tp_del slots, and stuff reachable
 * only from such cycles).
 * If DEBUG_SAVEALL, all objects in finalizers are appended to the module
 * garbage list (a Python list), else only the objects in finalizers with
 * __del__ methods are appended to garbage.  All objects in finalizers are
 * merged into the old list regardless.
 */
 // 定义一个静态函数 handle_legacy_finalizers，接受两个指向 PyGC_Head 结构体的指针
 // finalizers 链表包含具有旧版终结器的对象
 // old 链表用于合并 finalizers 链表中的对象
static void
handle_legacy_finalizers(PyGC_Head* finalizers, PyGC_Head* old)
{
    // 从 finalizers 链表的第一个对象开始遍历
    PyGC_Head* gc = finalizers->gc.gc_next;

    // 检查 _PyRuntime.gc.garbage 列表是否为空
    if (_PyRuntime.gc.garbage == NULL) {
        // 如果为空，创建一个新的空列表
        _PyRuntime.gc.garbage = PyList_New(0);
        // 检查列表是否创建成功
        if (_PyRuntime.gc.garbage == NULL)
            // 如果创建失败，触发致命错误
            Py_FatalError("gc couldn't create gc.garbage list");
    }

    // 遍历 finalizers 链表中的所有对象
    for (; gc != finalizers; gc = gc->gc.gc_next) {
        // 获取当前 PyGC_Head 对应的 Python 对象指针
        PyObject* op = FROM_GC(gc);

        // 检查是否开启了 DEBUG_SAVEALL 调试模式或者对象是否具有旧版终结器
        if ((_PyRuntime.gc.debug & DEBUG_SAVEALL) || has_legacy_finalizer(op)) {
            // 如果满足条件，将当前对象添加到 _PyRuntime.gc.garbage 列表中
            if (PyList_Append(_PyRuntime.gc.garbage, op) < 0)
                // 如果添加失败，跳出循环
                break;
        }
    }

    // 将 finalizers 链表合并到 old 链表中
    gc_list_merge(finalizers, old);
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
 // 定义一个静态函数 finalize_garbage，它接受一个指向 PyGC_Head 结构体的指针 collectable 作为参数
 // collectable 链表包含了待执行终结操作的对象
static void
finalize_garbage(PyGC_Head* collectable)
{
    // 声明一个 destructor 类型的变量 finalize
    // destructor 是一个函数指针类型，用于指向对象的 tp_finalize 方法，该方法用于执行对象的终结操作
    destructor finalize;
    // 定义一个 PyGC_Head 类型的变量 seen
    // 这个变量用于临时存放从 collectable 链表中取出的对象，以避免在终结操作过程中链表结构变化带来的问题
    PyGC_Head seen;

    /* While we're going through the loop, `finalize(op)` may cause op, or
     * other objects, to be reclaimed via refcounts falling to zero.  So
     * there's little we can rely on about the structure of the input
     * `collectable` list across iterations.  For safety, we always take the
     * first object in that list and move it to a temporary `seen` list.
     * If objects vanish from the `collectable` and `seen` lists we don't
     * care.
     */
     // 注释说明：在循环执行过程中，调用 `finalize(op)` 可能会导致对象 `op` 或其他对象因为引用计数降为零而被回收。
     // 所以在每次迭代中，`collectable` 链表的结构可能会发生很大变化，难以依赖其原本的结构。
     // 为了保证安全性，我们总是取出 `collectable` 链表的第一个对象，并将其移动到临时的 `seen` 链表中。
     // 如果对象从 `collectable` 和 `seen` 链表中消失，我们不需要关心。

     // 初始化 seen 链表
     // 确保 seen 链表的指针等信息正确设置，为后续存储对象做准备
    gc_list_init(&seen);

    // 当 collectable 链表不为空时，继续循环
    while (!gc_list_is_empty(collectable)) {
        // 获取 collectable 链表的第一个对象
        PyGC_Head* gc = collectable->gc.gc_next;
        // 获取当前 PyGC_Head 对应的 Python 对象指针
        PyObject* op = FROM_GC(gc);
        // 将当前对象从 collectable 链表移动到 seen 链表
        // 避免在执行终结操作时 collectable 链表结构混乱
        gc_list_move(gc, &seen);

        // 检查当前对象是否满足执行终结操作的条件
        // 条件 1：对象还未被终结过，即 _PyGCHead_FINALIZED(gc) 为 0
        // 条件 2：对象的类型具有终结器特性，即 PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_HAVE_FINALIZE) 为真
        // 条件 3：对象的类型定义了 tp_finalize 方法，即 Py_TYPE(op)->tp_finalize 不为 NULL
        if (!_PyGCHead_FINALIZED(gc) &&
            PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_HAVE_FINALIZE) &&
            (finalize = Py_TYPE(op)->tp_finalize) != NULL) {
            // 标记当前对象已经被终结
            _PyGCHead_SET_FINALIZED(gc, 1);
            // 增加对象的引用计数
            // 防止在执行终结操作过程中对象因为引用计数降为零而被提前回收
            Py_INCREF(op);
            // 调用对象的终结器方法
            finalize(op);
            // 减少对象的引用计数
            // 恢复对象原本的引用计数状态
            Py_DECREF(op);
        }
    }

    // 将 seen 链表中的对象合并回 collectable 链表
    // 完成终结操作后，恢复 collectable 链表的完整性
    gc_list_merge(&seen, collectable);
}

/* Walk the collectable list and check that they are really unreachable
   from the outside (some objects could have been resurrected by a
   finalizer). */
   // 定义一个静态函数 check_garbage，它接受一个指向 PyGC_Head 结构体的指针 collectable 作为参数
   // collectable 链表包含了待检查是否为垃圾对象的对象
static int
check_garbage(PyGC_Head* collectable)
{
    // 定义一个指向 PyGC_Head 结构体的指针 gc，用于遍历 collectable 链表
    PyGC_Head* gc;

    // 第一个循环：更新对象的引用计数
    // 从 collectable 链表的第一个对象开始遍历，直到回到链表头（collectable）
    for (gc = collectable->gc.gc_next; gc != collectable;
        gc = gc->gc.gc_next) {
        // 将当前对象的 gc_refs 字段设置为该对象的实际引用计数
        // FROM_GC(gc) 用于从 PyGC_Head 指针获取对应的 Python 对象指针
        // Py_REFCNT 用于获取该对象的引用计数
        _PyGCHead_SET_REFS(gc, Py_REFCNT(FROM_GC(gc)));
        // 断言当前对象的引用计数不为 0
        // 确保对象在更新引用计数后，其引用计数是有效的
        assert(_PyGCHead_REFS(gc) != 0);
    }

    // 调用 subtract_refs 函数，减去对象之间的循环引用计数
    // 该函数会遍历 collectable 链表中的每个对象，调用其 tp_traverse 方法，
    // 并使用 visit_decref 作为访问函数，从而减去循环引用计数
    subtract_refs(collectable);

    // 第二个循环：检查剩余引用计数
    // 再次从 collectable 链表的第一个对象开始遍历，直到回到链表头（collectable）
    for (gc = collectable->gc.gc_next; gc != collectable;
        gc = gc->gc.gc_next) {
        // 断言当前对象的剩余引用计数大于等于 0
        // 确保减去循环引用计数后，引用计数不会为负数
        assert(_PyGCHead_REFS(gc) >= 0);
        // 如果当前对象的剩余引用计数不为 0，说明该对象还有外部引用，不是垃圾对象
        if (_PyGCHead_REFS(gc) != 0)
            // 返回 -1 表示检查失败，存在非垃圾对象
            return -1;
    }

    // 如果所有对象的剩余引用计数都为 0，说明 collectable 链表中的对象都是垃圾对象
    // 返回 0 表示检查成功
    return 0;
}

// 定义一个静态函数 revive_garbage，它接受一个指向 PyGC_Head 结构体的指针 collectable 作为参数
// collectable 链表包含了那些可能被错误标记为不可达的对象，需要将它们恢复为可达状态
static void
revive_garbage(PyGC_Head* collectable)
{
    // 定义一个指向 PyGC_Head 结构体的指针 gc，用于遍历 collectable 链表
    PyGC_Head* gc;

    // 遍历 collectable 链表，从链表的第一个对象开始，直到回到链表头（collectable）
    for (gc = collectable->gc.gc_next; gc != collectable;
        gc = gc->gc.gc_next) {
        // 将当前对象的 gc_refs 字段设置为 GC_REACHABLE，表示该对象是可达的
        // 这样做是为了撤销之前可能的不可达标记，恢复对象的正常状态
        _PyGCHead_SET_REFS(gc, GC_REACHABLE);
    }
}

/* Break reference cycles by clearing the containers involved.  This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
 // 定义一个静态函数 delete_garbage，它接受两个指向 PyGC_Head 结构体的指针 collectable 和 old 作为参数
 // collectable 链表包含了待处理的垃圾对象
 // old 链表用于存放那些在清除操作后仍然存活的对象
static void
delete_garbage(PyGC_Head* collectable, PyGC_Head* old)
{
    // 声明一个 inquiry 类型的变量 clear
    // inquiry 是一个函数指针类型，用于指向对象的 tp_clear 方法，该方法用于清除对象的引用
    inquiry clear;

    // 当 collectable 链表不为空时，继续循环处理其中的对象
    while (!gc_list_is_empty(collectable)) {
        // 获取 collectable 链表的第一个对象
        PyGC_Head* gc = collectable->gc.gc_next;
        // 获取当前 PyGC_Head 对应的 Python 对象指针
        PyObject* op = FROM_GC(gc);

        // 检查是否开启了 DEBUG_SAVEALL 调试模式
        if (_PyRuntime.gc.debug & DEBUG_SAVEALL) {
            // 如果开启了 DEBUG_SAVEALL 调试模式，将当前对象添加到 _PyRuntime.gc.garbage 列表中
            // 这样做是为了在调试时保留这些垃圾对象，方便后续分析
            PyList_Append(_PyRuntime.gc.garbage, op);
        }
        else {
            // 如果没有开启 DEBUG_SAVEALL 调试模式，尝试清除对象的引用
            // 获取对象类型的 tp_clear 方法
            if ((clear = Py_TYPE(op)->tp_clear) != NULL) {
                // 增加对象的引用计数，防止在清除操作过程中对象被提前回收
                Py_INCREF(op);
                // 调用对象的 tp_clear 方法，清除对象的引用
                clear(op);
                // 减少对象的引用计数，恢复对象原本的引用计数状态
                Py_DECREF(op);
            }
        }

        // 检查对象是否仍然存活
        // 如果 collectable 链表的下一个对象仍然是当前对象，说明对象在清除操作后仍然存活
        if (collectable->gc.gc_next == gc) {
            // 注释说明：对象仍然存活，将其移动到 old 链表中，它可能在后续操作中被销毁
            // 将当前对象从 collectable 链表移动到 old 链表
            gc_list_move(gc, old);
            // 将该对象的引用计数状态标记为可达
            // 因为对象仍然存活，所以暂时认为它是可达的
            _PyGCHead_SET_REFS(gc, GC_REACHABLE);
        }
    }
}

/* Clear all free lists
 * All free lists are cleared during the collection of the highest generation.
 * Allocated items in the free list may keep a pymalloc arena occupied.
 * Clearing the free lists may give back memory to the OS earlier.
 */
static void
clear_freelists(void)
{
    (void)PyMethod_ClearFreeList();
    (void)PyFrame_ClearFreeList();
    (void)PyCFunction_ClearFreeList();
    (void)PyTuple_ClearFreeList();
    (void)PyUnicode_ClearFreeList();
    (void)PyFloat_ClearFreeList();
    (void)PyList_ClearFreeList();
    (void)PyDict_ClearFreeList();
    (void)PySet_ClearFreeList();
    (void)PyAsyncGen_ClearFreeLists();
    (void)PyContext_ClearFreeList();
}

/* This is the main function.  Read this to understand how the
 * collection process works. */

 /**
  * collect 函数是 Python 垃圾回收机制的核心函数，用于执行指定代数的垃圾回收操作。
  * 它会标记并回收那些不可达的对象，同时处理包含终结器的对象，避免内存泄漏。
  *
  * @param generation 要进行垃圾回收的代数。代数越高，对象存活时间越长，回收频率越低。
  * @param n_collected 指向 Py_ssize_t 类型的指针，用于存储成功回收的对象数量。
  * @param n_uncollectable 指向 Py_ssize_t 类型的指针，用于存储无法回收的对象数量。
  * @param nofail 一个标志，指示在垃圾回收过程中发生错误时是否忽略错误。
  * @return 返回成功回收和无法回收的对象总数。
    准备工作：记录调试信息、更新计数器、合并更年轻代数的对象链表。
    标记可达对象：通过更新和减去引用计数，标记出从外部可达的对象。
    分离不可达对象：将不可达对象移动到 unreachable 链表。
    处理可达对象：将可达对象移动到下一代或进行特殊处理。
    处理终结器：分离出带有旧版终结器的不可达对象。
    清理垃圾：处理弱引用、调用终结器、打破引用循环并删除垃圾对象。
    统计和调试：统计不可收集对象数量，输出调试信息。
    异常处理：处理垃圾回收过程中发生的异常。
    更新统计信息：更新当前代数的收集统计信息。
    结束操作：触发 DTrace 探针，返回回收和不可收集对象总数。
  */
static Py_ssize_t
collect(int generation, Py_ssize_t* n_collected, Py_ssize_t* n_uncollectable,
    int nofail)
{
    int i;
    // m 用于记录成功回收的对象数量
    Py_ssize_t m = 0;
    // n 用于记录无法回收的对象数量
    Py_ssize_t n = 0;
    // young 指向当前要检查的代数的对象链表头
    PyGC_Head* young;
    // old 指向下一个更老代数的对象链表头
    PyGC_Head* old;
    // unreachable 用于存储不可达且无问题的垃圾对象
    PyGC_Head unreachable;
    // finalizers 用于存储带有 __del__ 方法以及从这些对象可达的对象
    PyGC_Head finalizers;
    PyGC_Head* gc;
    // t1 用于记录垃圾回收开始时间，避免编译器警告先初始化为 0
    _PyTime_t t1 = 0;

    // 获取当前代数的统计信息结构体指针
    struct gc_generation_stats* stats = &_PyRuntime.gc.generation_stats[generation];

    // 如果开启了 DEBUG_STATS 调试模式，输出垃圾回收开始信息和各代对象数量
    if (_PyRuntime.gc.debug & DEBUG_STATS) {
        PySys_WriteStderr("gc: collecting generation %d...\n",
            generation);
        PySys_WriteStderr("gc: objects in each generation:");
        for (i = 0; i < NUM_GENERATIONS; i++)
            PySys_FormatStderr(" %zd",
                gc_list_size(GEN_HEAD(i)));
        PySys_WriteStderr("\ngc: objects in permanent generation: %zd",
            gc_list_size(&_PyRuntime.gc.permanent_generation.head));
        // 记录垃圾回收开始时间
        t1 = _PyTime_GetMonotonicClock();

        PySys_WriteStderr("\n");
    }

    // 如果 DTrace 启用了 GC_START 探针，触发该探针
    if (PyDTrace_GC_START_ENABLED())
        PyDTrace_GC_START(generation);

    /* 更新收集和分配计数器 */
    // 如果当前代数不是最高代数，将下一个更老代数的计数加 1
    if (generation + 1 < NUM_GENERATIONS)
        _PyRuntime.gc.generations[generation + 1].count += 1;
    // 将当前代数及更年轻代数的计数清零
    for (i = 0; i <= generation; i++)
        _PyRuntime.gc.generations[i].count = 0;

    /* 将更年轻的代数合并到当前要收集的代数中 */
    for (i = 0; i < generation; i++) {
        // 调用 gc_list_merge 函数将更年轻代数的对象链表合并到当前代数
        gc_list_merge(GEN_HEAD(i), GEN_HEAD(generation));
    }

    // 获取当前要检查的代数的对象链表头
    young = GEN_HEAD(generation);
    // 如果当前代数不是最高代数，获取下一个更老代数的对象链表头
    if (generation < NUM_GENERATIONS - 1)
        old = GEN_HEAD(generation + 1);
    else
        // 如果是最高代数，old 指向 young
        old = young;

    /* 使用 ob_refcnt 和 gc_refs，计算容器集合中哪些对象可以从集合外部访问
     * （即当考虑集合内的所有引用时，引用计数大于 0）。
     */
     // 更新引用计数
    update_refs(young);
    // 减去集合内的引用计数
    subtract_refs(young);

    /* 将从 young 外部可达的所有对象留在 young 中，并将其他所有对象（在 young 中）移动到 unreachable 中。
     * 注意：以前是将可达对象移动到可达集合中。但通常大多数对象都是可达的，
     * 所以移动不可达对象更高效。
     */
     // 初始化 unreachable 链表
    gc_list_init(&unreachable);
    // 将不可达对象从 young 移动到 unreachable
    move_unreachable(young, &unreachable);

    /* 将可达对象移动到下一代 */
    if (young != old) {
        if (generation == NUM_GENERATIONS - 2) {
            // 如果是倒数第二代，增加长生命周期待处理对象的计数
            _PyRuntime.gc.long_lived_pending += gc_list_size(young);
        }
        // 将 young 中的可达对象合并到 old 中
        gc_list_merge(young, old);
    }
    else {
        /* 我们只在完整收集时取消跟踪字典，以避免二次字典堆积。参见问题 #14775。 */
        // 取消跟踪字典
        untrack_dicts(young);
        // 重置长生命周期待处理对象计数
        _PyRuntime.gc.long_lived_pending = 0;
        // 更新长生命周期对象总数
        _PyRuntime.gc.long_lived_total = gc_list_size(young);
    }

    /* unreachable 中的所有对象都是垃圾，但从旧版终结器（例如 tp_del）可达的对象不能安全删除。 */
    // 初始化 finalizers 链表
    gc_list_init(&finalizers);
    // 将带有旧版终结器的不可达对象从 unreachable 移动到 finalizers
    move_legacy_finalizers(&unreachable, &finalizers);
    /* finalizers 包含带有旧版终结器的不可达对象；
     * 从这些对象可达的不可达对象也是不可收集的，我们也将它们移动到 finalizers 列表中。
     */
     // 移动从 finalizers 可达的不可收集对象到 finalizers
    move_legacy_finalizer_reachable(&finalizers);

    // 如果开启了 DEBUG_COLLECTABLE 调试模式，输出可收集对象信息
    if (_PyRuntime.gc.debug & DEBUG_COLLECTABLE) {
        for (gc = unreachable.gc.gc_next; gc != &unreachable; gc = gc->gc.gc_next) {
            debug_cycle("collectable", FROM_GC(gc));
        }
    }

    /* 必要时清除弱引用并调用回调函数 */
    // 处理弱引用，返回成功处理的对象数量并累加到 m
    m += handle_weakrefs(&unreachable, old);

    /* 对有 tp_finalize 的对象调用 tp_finalize */
    // 对 unreachable 中的对象调用终结器
    finalize_garbage(&unreachable);

    // 检查垃圾对象是否可以被回收
    if (check_garbage(&unreachable)) {
        // 如果不能回收，将其复活并合并到 old 中
        revive_garbage(&unreachable);
        gc_list_merge(&unreachable, old);
    }
    else {
        /* 对 unreachable 集合中的对象调用 tp_clear。这将导致引用循环被打破。
         * 这也可能导致 finalizers 中的一些对象被释放。
         */
         // 增加 unreachable 中对象数量到 m
        m += gc_list_size(&unreachable);
        // 删除 unreachable 中的垃圾对象
        delete_garbage(&unreachable, old);
    }

    /* 收集找到的不可收集对象的统计信息并输出调试信息 */
    for (gc = finalizers.gc.gc_next;
        gc != &finalizers;
        gc = gc->gc.gc_next) {
        // 统计不可收集对象数量
        n++;
        if (_PyRuntime.gc.debug & DEBUG_UNCOLLECTABLE)
            // 如果开启 DEBUG_UNCOLLECTABLE 调试模式，输出不可收集对象信息
            debug_cycle("uncollectable", FROM_GC(gc));
    }
    if (_PyRuntime.gc.debug & DEBUG_STATS) {
        // 记录垃圾回收结束时间
        _PyTime_t t2 = _PyTime_GetMonotonicClock();

        if (m == 0 && n == 0)
            PySys_WriteStderr("gc: done");
        else
            PySys_FormatStderr(
                "gc: done, %zd unreachable, %zd uncollectable",
                n + m, n);
        // 输出垃圾回收耗时
        PySys_WriteStderr(", %.4fs elapsed\n",
            _PyTime_AsSecondsDouble(t2 - t1));
    }

    /* 将不可收集集合中的实例附加到 Python 可达的垃圾列表中。
     * 如果程序员坚持创建这种类型的结构，他们必须处理这个问题。
     */
     // 处理旧版终结器
    handle_legacy_finalizers(&finalizers, old);

    /* 仅在收集最高代数时清除空闲列表 */
    if (generation == NUM_GENERATIONS - 1) {
        // 清除空闲列表
        clear_freelists();
    }

    // 检查垃圾回收过程中是否发生异常
    if (PyErr_Occurred()) {
        if (nofail) {
            // 如果 nofail 为真，清除异常信息
            PyErr_Clear();
        }
        else {
            if (gc_str == NULL)
                // 创建一个 Unicode 对象表示 "garbage collection"
                gc_str = PyUnicode_FromString("garbage collection");
            // 输出未捕获异常信息
            PyErr_WriteUnraisable(gc_str);
            // 抛出致命错误
            Py_FatalError("unexpected exception during garbage collection");
        }
    }

    /* 更新统计信息 */
    if (n_collected)
        // 如果 n_collected 不为空，将成功回收的对象数量赋值给它
        *n_collected = m;
    if (n_uncollectable)
        // 如果 n_uncollectable 不为空，将无法回收的对象数量赋值给它
        *n_uncollectable = n;
    // 增加当前代数的收集次数
    stats->collections++;
    // 累加成功回收的对象数量
    stats->collected += m;
    // 累加无法回收的对象数量
    stats->uncollectable += n;

    // 如果 DTrace 启用了 GC_DONE 探针，触发该探针
    if (PyDTrace_GC_DONE_ENABLED())
        PyDTrace_GC_DONE(n + m);

    // 返回成功回收和无法回收的对象总数
    return n + m;
}

/* Invoke progress callbacks to notify clients that garbage collection
 * is starting or stopping
 */
static void
invoke_gc_callback(const char *phase, int generation,
                   Py_ssize_t collected, Py_ssize_t uncollectable)
{
    Py_ssize_t i;
    PyObject *info = NULL;

    /* we may get called very early */
    if (_PyRuntime.gc.callbacks == NULL)
        return;
    /* The local variable cannot be rebound, check it for sanity */
    assert(_PyRuntime.gc.callbacks != NULL && PyList_CheckExact(_PyRuntime.gc.callbacks));
    if (PyList_GET_SIZE(_PyRuntime.gc.callbacks) != 0) {
        info = Py_BuildValue("{sisnsn}",
            "generation", generation,
            "collected", collected,
            "uncollectable", uncollectable);
        if (info == NULL) {
            PyErr_WriteUnraisable(NULL);
            return;
        }
    }
    for (i=0; i<PyList_GET_SIZE(_PyRuntime.gc.callbacks); i++) {
        PyObject *r, *cb = PyList_GET_ITEM(_PyRuntime.gc.callbacks, i);
        Py_INCREF(cb); /* make sure cb doesn't go away */
        r = PyObject_CallFunction(cb, "sO", phase, info);
        if (r == NULL) {
            PyErr_WriteUnraisable(cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_XDECREF(info);
}

/* Perform garbage collection of a generation and invoke
 * progress callbacks.
 */
 /**
  * collect_with_callback 函数用于执行垃圾回收操作，并在操作前后调用回调函数，以记录或处理垃圾回收的相关信息。
  *
  * @param generation 一个整数，表示要进行垃圾回收的起始代数。垃圾回收通常会从指定代数开始，并可能涉及更年轻的代数。
  * @return 返回一个 Py_ssize_t 类型的值，表示垃圾回收操作的结果，通常是回收的对象数量。
  */
static Py_ssize_t
collect_with_callback(int generation)
{
    // 定义 Py_ssize_t 类型的变量，用于存储不同的垃圾回收结果信息
    // result 用于存储垃圾回收操作的最终结果，通常是回收的对象数量
    Py_ssize_t result;
    // collected 用于存储本次垃圾回收中成功回收的对象数量
    Py_ssize_t collected;
    // uncollectable 用于存储本次垃圾回收中无法回收的对象数量，这些对象可能由于存在循环引用等原因无法被正常回收
    Py_ssize_t uncollectable;

    // 调用 invoke_gc_callback 函数，在垃圾回收开始前触发回调。
    // 第一个参数 "start" 是回调的标识符，表示垃圾回收操作开始
    // 第二个参数 generation 传递要进行垃圾回收的起始代数
    // 第三个和第四个参数都为 0，因为在开始阶段还没有实际的回收数量信息
    invoke_gc_callback("start", generation, 0, 0);

    // 调用 collect 函数执行实际的垃圾回收操作。
    // 第一个参数 generation 表示从哪个代数开始进行垃圾回收
    // 第二个参数 &collected 是一个指向 collected 变量的指针，用于让 collect 函数将成功回收的对象数量存储到该变量中
    // 第三个参数 &uncollectable 是一个指向 uncollectable 变量的指针，用于让 collect 函数将无法回收的对象数量存储到该变量中
    // 第四个参数 0 可能是一个标志位，用于传递额外的回收选项，但这里传递 0 表示使用默认选项
    result = collect(generation, &collected, &uncollectable, 0);

    // 调用 invoke_gc_callback 函数，在垃圾回收结束后触发回调。
    // 第一个参数 "stop" 是回调的标识符，表示垃圾回收操作结束
    // 第二个参数 generation 传递本次进行垃圾回收的起始代数
    // 第三个参数 collected 传递本次垃圾回收中成功回收的对象数量
    // 第四个参数 uncollectable 传递本次垃圾回收中无法回收的对象数量
    invoke_gc_callback("stop", generation, collected, uncollectable);

    // 返回垃圾回收操作的最终结果，即回收的对象数量
    return result;
}

// 该函数用于触发 Python 垃圾回收机制中的分代回收操作，
// 会根据各代的对象计数和阈值情况，决定对哪些代的对象进行垃圾回收。
// 返回值是本次垃圾回收过程中回收的对象数量。
static Py_ssize_t
collect_generations(void)
{
    // 定义一个整数变量 i，用于遍历不同的垃圾回收代。
    int i;
    // 定义一个 Py_ssize_t 类型的变量 n，用于记录本次垃圾回收过程中回收的对象数量，初始化为 0。
    Py_ssize_t n = 0;
    /*
     * 查找对象计数超过阈值的最老的代（编号最大的代）。
     * 该代以及比它年轻的代中的对象都将被回收。
     * 从最老的代开始向前遍历，因为分代回收机制中，
     * 当较老的代需要回收时，通常也会连带回收较年轻代的对象。
     */
    for (i = NUM_GENERATIONS - 1; i >= 0; i--) {
        // 检查当前代的对象计数是否超过了该代的阈值。
        if (_PyRuntime.gc.generations[i].count > _PyRuntime.gc.generations[i].threshold) {
            /*
             * 避免在跟踪对象数量较多时出现二次性能退化问题。
             * 参考文件开头的注释和问题 #4074 了解更多信息。
             * 如果当前是最老的代（i 等于 NUM_GENERATIONS - 1），
             * 并且长生命周期待处理对象的数量小于长生命周期对象总数的四分之一，
             * 则跳过本次回收，继续检查下一个较年轻的代。
             * 这是一种性能优化策略，防止不必要的垃圾回收操作。
             */
            if (i == NUM_GENERATIONS - 1
                && _PyRuntime.gc.long_lived_pending < _PyRuntime.gc.long_lived_total / 4)
                continue;
            // 调用 collect_with_callback 函数对从第 i 代及更年轻的代进行垃圾回收操作，
            // 并将回收的对象数量赋值给变量 n。
            // collect_with_callback 函数可能会执行一些回调操作，
            // 例如在回收前后执行特定的函数。
            n = collect_with_callback(i);
            // 一旦找到需要回收的代并执行了回收操作，就跳出循环，
            // 因为已经对相关代的对象进行了回收。
            break;
        }
    }
    // 返回本次垃圾回收过程中回收的对象数量。
    return n;
}

#include "clinic/gcmodule.c.h"

/*[clinic input]
gc.enable

Enable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_enable_impl(PyObject *module)
/*[clinic end generated code: output=45a427e9dce9155c input=81ac4940ca579707]*/
{
    _PyRuntime.gc.enabled = 1;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.disable

Disable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_disable_impl(PyObject *module)
/*[clinic end generated code: output=97d1030f7aa9d279 input=8c2e5a14e800d83b]*/
{
    _PyRuntime.gc.enabled = 0;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.isenabled -> bool

Returns true if automatic garbage collection is enabled.
[clinic start generated code]*/

static int
gc_isenabled_impl(PyObject *module)
/*[clinic end generated code: output=1874298331c49130 input=30005e0422373b31]*/
{
    return _PyRuntime.gc.enabled;
}

/*[clinic input]
gc.collect -> Py_ssize_t

    generation: int(c_default="NUM_GENERATIONS - 1") = 2

Run the garbage collector.

With no arguments, run a full collection.  The optional argument
may be an integer specifying which generation to collect.  A ValueError
is raised if the generation number is invalid.

The number of unreachable objects is returned.
[clinic start generated code]*/

static Py_ssize_t
gc_collect_impl(PyObject *module, int generation)
/*[clinic end generated code: output=b697e633043233c7 input=40720128b682d879]*/
{
    Py_ssize_t n;

    if (generation < 0 || generation >= NUM_GENERATIONS) {
        PyErr_SetString(PyExc_ValueError, "invalid generation");
        return -1;
    }

    if (_PyRuntime.gc.collecting)
        n = 0; /* already collecting, don't do anything */
    else {
        _PyRuntime.gc.collecting = 1;
        n = collect_with_callback(generation);
        _PyRuntime.gc.collecting = 0;
    }

    return n;
}

/*[clinic input]
gc.set_debug

    flags: int
        An integer that can have the following bits turned on:
          DEBUG_STATS - Print statistics during collection.
          DEBUG_COLLECTABLE - Print collectable objects found.
          DEBUG_UNCOLLECTABLE - Print unreachable but uncollectable objects
            found.
          DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.
          DEBUG_LEAK - Debug leaking programs (everything but STATS).
    /

Set the garbage collection debugging flags.

Debugging information is written to sys.stderr.
[clinic start generated code]*/

static PyObject *
gc_set_debug_impl(PyObject *module, int flags)
/*[clinic end generated code: output=7c8366575486b228 input=5e5ce15e84fbed15]*/
{
    _PyRuntime.gc.debug = flags;

    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_debug -> int

Get the garbage collection debugging flags.
[clinic start generated code]*/

static int
gc_get_debug_impl(PyObject *module)
/*[clinic end generated code: output=91242f3506cd1e50 input=91a101e1c3b98366]*/
{
    return _PyRuntime.gc.debug;
}

PyDoc_STRVAR(gc_set_thresh__doc__,
"set_threshold(threshold0, [threshold1, threshold2]) -> None\n"
"\n"
"Sets the collection thresholds.  Setting threshold0 to zero disables\n"
"collection.\n");

static PyObject *
gc_set_thresh(PyObject *self, PyObject *args)
{
    int i;
    if (!PyArg_ParseTuple(args, "i|ii:set_threshold",
                          &_PyRuntime.gc.generations[0].threshold,
                          &_PyRuntime.gc.generations[1].threshold,
                          &_PyRuntime.gc.generations[2].threshold))
        return NULL;
    for (i = 2; i < NUM_GENERATIONS; i++) {
        /* generations higher than 2 get the same threshold */
        _PyRuntime.gc.generations[i].threshold = _PyRuntime.gc.generations[2].threshold;
    }

    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_threshold

Return the current collection thresholds.
[clinic start generated code]*/

static PyObject *
gc_get_threshold_impl(PyObject *module)
/*[clinic end generated code: output=7902bc9f41ecbbd8 input=286d79918034d6e6]*/
{
    return Py_BuildValue("(iii)",
                         _PyRuntime.gc.generations[0].threshold,
                         _PyRuntime.gc.generations[1].threshold,
                         _PyRuntime.gc.generations[2].threshold);
}

/*[clinic input]
gc.get_count

Return a three-tuple of the current collection counts.
[clinic start generated code]*/

static PyObject *
gc_get_count_impl(PyObject *module)
/*[clinic end generated code: output=354012e67b16398f input=a392794a08251751]*/
{
    return Py_BuildValue("(iii)",
                         _PyRuntime.gc.generations[0].count,
                         _PyRuntime.gc.generations[1].count,
                         _PyRuntime.gc.generations[2].count);
}

static int
referrersvisit(PyObject* obj, PyObject *objs)
{
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(objs); i++)
        if (PyTuple_GET_ITEM(objs, i) == obj)
            return 1;
    return 0;
}

static int
gc_referrers_for(PyObject *objs, PyGC_Head *list, PyObject *resultlist)
{
    PyGC_Head *gc;
    PyObject *obj;
    traverseproc traverse;
    for (gc = list->gc.gc_next; gc != list; gc = gc->gc.gc_next) {
        obj = FROM_GC(gc);
        traverse = Py_TYPE(obj)->tp_traverse;
        if (obj == objs || obj == resultlist)
            continue;
        if (traverse(obj, (visitproc)referrersvisit, objs)) {
            if (PyList_Append(resultlist, obj) < 0)
                return 0; /* error */
        }
    }
    return 1; /* no error */
}

PyDoc_STRVAR(gc_get_referrers__doc__,
"get_referrers(*objs) -> list\n\
Return the list of objects that directly refer to any of objs.");

static PyObject *
gc_get_referrers(PyObject *self, PyObject *args)
{
    int i;
    PyObject *result = PyList_New(0);
    if (!result) return NULL;

    for (i = 0; i < NUM_GENERATIONS; i++) {
        if (!(gc_referrers_for(args, GEN_HEAD(i), result))) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

/* Append obj to list; return true if error (out of memory), false if OK. */
static int
referentsvisit(PyObject *obj, PyObject *list)
{
    return PyList_Append(list, obj) < 0;
}

PyDoc_STRVAR(gc_get_referents__doc__,
"get_referents(*objs) -> list\n\
Return the list of objects that are directly referred to by objs.");

static PyObject *
gc_get_referents(PyObject *self, PyObject *args)
{
    Py_ssize_t i;
    PyObject *result = PyList_New(0);

    if (result == NULL)
        return NULL;

    for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
        traverseproc traverse;
        PyObject *obj = PyTuple_GET_ITEM(args, i);

        if (! PyObject_IS_GC(obj))
            continue;
        traverse = Py_TYPE(obj)->tp_traverse;
        if (! traverse)
            continue;
        if (traverse(obj, (visitproc)referentsvisit, result)) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

/*[clinic input]
gc.get_objects

Return a list of objects tracked by the collector (excluding the list returned).
[clinic start generated code]*/

static PyObject *
gc_get_objects_impl(PyObject *module)
/*[clinic end generated code: output=fcb95d2e23e1f750 input=9439fe8170bf35d8]*/
{
    int i;
    PyObject* result;

    result = PyList_New(0);
    if (result == NULL)
        return NULL;
    for (i = 0; i < NUM_GENERATIONS; i++) {
        if (append_objects(result, GEN_HEAD(i))) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

/*[clinic input]
gc.get_stats

Return a list of dictionaries containing per-generation statistics.
[clinic start generated code]*/

static PyObject *
gc_get_stats_impl(PyObject *module)
/*[clinic end generated code: output=a8ab1d8a5d26f3ab input=1ef4ed9d17b1a470]*/
{
    int i;
    PyObject *result;
    struct gc_generation_stats stats[NUM_GENERATIONS], *st;

    /* To get consistent values despite allocations while constructing
       the result list, we use a snapshot of the running stats. */
    for (i = 0; i < NUM_GENERATIONS; i++) {
        stats[i] = _PyRuntime.gc.generation_stats[i];
    }

    result = PyList_New(0);
    if (result == NULL)
        return NULL;

    for (i = 0; i < NUM_GENERATIONS; i++) {
        PyObject *dict;
        st = &stats[i];
        dict = Py_BuildValue("{snsnsn}",
                             "collections", st->collections,
                             "collected", st->collected,
                             "uncollectable", st->uncollectable
                            );
        if (dict == NULL)
            goto error;
        if (PyList_Append(result, dict)) {
            Py_DECREF(dict);
            goto error;
        }
        Py_DECREF(dict);
    }
    return result;

error:
    Py_XDECREF(result);
    return NULL;
}


/*[clinic input]
gc.is_tracked

    obj: object
    /

Returns true if the object is tracked by the garbage collector.

Simple atomic objects will return false.
[clinic start generated code]*/

static PyObject *
gc_is_tracked(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=14f0103423b28e31 input=d83057f170ea2723]*/
{
    PyObject *result;

    if (PyObject_IS_GC(obj) && IS_TRACKED(obj))
        result = Py_True;
    else
        result = Py_False;
    Py_INCREF(result);
    return result;
}

/*[clinic input]
gc.freeze

Freeze all current tracked objects and ignore them for future collections.

This can be used before a POSIX fork() call to make the gc copy-on-write friendly.
Note: collection before a POSIX fork() call may free pages for future allocation
which can cause copy-on-write.
[clinic start generated code]*/

static PyObject *
gc_freeze_impl(PyObject *module)
/*[clinic end generated code: output=502159d9cdc4c139 input=b602b16ac5febbe5]*/
{
    for (int i = 0; i < NUM_GENERATIONS; ++i) {
        gc_list_merge(GEN_HEAD(i), &_PyRuntime.gc.permanent_generation.head);
        _PyRuntime.gc.generations[i].count = 0;
    }
    Py_RETURN_NONE;
}

/*[clinic input]
gc.unfreeze

Unfreeze all objects in the permanent generation.

Put all objects in the permanent generation back into oldest generation.
[clinic start generated code]*/

static PyObject *
gc_unfreeze_impl(PyObject *module)
/*[clinic end generated code: output=1c15f2043b25e169 input=2dd52b170f4cef6c]*/
{
    gc_list_merge(&_PyRuntime.gc.permanent_generation.head, GEN_HEAD(NUM_GENERATIONS-1));
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_freeze_count -> Py_ssize_t

Return the number of objects in the permanent generation.
[clinic start generated code]*/

static Py_ssize_t
gc_get_freeze_count_impl(PyObject *module)
/*[clinic end generated code: output=61cbd9f43aa032e1 input=45ffbc65cfe2a6ed]*/
{
    return gc_list_size(&_PyRuntime.gc.permanent_generation.head);
}


PyDoc_STRVAR(gc__doc__,
"This module provides access to the garbage collector for reference cycles.\n"
"\n"
"enable() -- Enable automatic garbage collection.\n"
"disable() -- Disable automatic garbage collection.\n"
"isenabled() -- Returns true if automatic collection is enabled.\n"
"collect() -- Do a full collection right now.\n"
"get_count() -- Return the current collection counts.\n"
"get_stats() -- Return list of dictionaries containing per-generation stats.\n"
"set_debug() -- Set debugging flags.\n"
"get_debug() -- Get debugging flags.\n"
"set_threshold() -- Set the collection thresholds.\n"
"get_threshold() -- Return the current the collection thresholds.\n"
"get_objects() -- Return a list of all objects tracked by the collector.\n"
"is_tracked() -- Returns true if a given object is tracked.\n"
"get_referrers() -- Return the list of objects that refer to an object.\n"
"get_referents() -- Return the list of objects that an object refers to.\n"
"freeze() -- Freeze all tracked objects and ignore them for future collections.\n"
"unfreeze() -- Unfreeze all objects in the permanent generation.\n"
"get_freeze_count() -- Return the number of objects in the permanent generation.\n");

static PyMethodDef GcMethods[] = {
    GC_ENABLE_METHODDEF
    GC_DISABLE_METHODDEF
    GC_ISENABLED_METHODDEF
    GC_SET_DEBUG_METHODDEF
    GC_GET_DEBUG_METHODDEF
    GC_GET_COUNT_METHODDEF
    {"set_threshold",  gc_set_thresh, METH_VARARGS, gc_set_thresh__doc__},
    GC_GET_THRESHOLD_METHODDEF
    GC_COLLECT_METHODDEF
    GC_GET_OBJECTS_METHODDEF
    GC_GET_STATS_METHODDEF
    GC_IS_TRACKED_METHODDEF
    {"get_referrers",  gc_get_referrers, METH_VARARGS,
        gc_get_referrers__doc__},
    {"get_referents",  gc_get_referents, METH_VARARGS,
        gc_get_referents__doc__},
    GC_FREEZE_METHODDEF
    GC_UNFREEZE_METHODDEF
    GC_GET_FREEZE_COUNT_METHODDEF
    {NULL,      NULL}           /* Sentinel */
};

static struct PyModuleDef gcmodule = {
    PyModuleDef_HEAD_INIT,
    "gc",              /* m_name */
    gc__doc__,         /* m_doc */
    -1,                /* m_size */
    GcMethods,         /* m_methods */
    NULL,              /* m_reload */
    NULL,              /* m_traverse */
    NULL,              /* m_clear */
    NULL               /* m_free */
};

PyMODINIT_FUNC
PyInit_gc(void)
{
    PyObject *m;

    m = PyModule_Create(&gcmodule);

    if (m == NULL)
        return NULL;

    if (_PyRuntime.gc.garbage == NULL) {
        _PyRuntime.gc.garbage = PyList_New(0);
        if (_PyRuntime.gc.garbage == NULL)
            return NULL;
    }
    Py_INCREF(_PyRuntime.gc.garbage);
    if (PyModule_AddObject(m, "garbage", _PyRuntime.gc.garbage) < 0)
        return NULL;

    if (_PyRuntime.gc.callbacks == NULL) {
        _PyRuntime.gc.callbacks = PyList_New(0);
        if (_PyRuntime.gc.callbacks == NULL)
            return NULL;
    }
    Py_INCREF(_PyRuntime.gc.callbacks);
    if (PyModule_AddObject(m, "callbacks", _PyRuntime.gc.callbacks) < 0)
        return NULL;

#define ADD_INT(NAME) if (PyModule_AddIntConstant(m, #NAME, NAME) < 0) return NULL
    ADD_INT(DEBUG_STATS);
    ADD_INT(DEBUG_COLLECTABLE);
    ADD_INT(DEBUG_UNCOLLECTABLE);
    ADD_INT(DEBUG_SAVEALL);
    ADD_INT(DEBUG_LEAK);
#undef ADD_INT
    return m;
}

/* API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
    Py_ssize_t n;

    if (_PyRuntime.gc.collecting)
        n = 0; /* already collecting, don't do anything */
    else {
        PyObject *exc, *value, *tb;
        _PyRuntime.gc.collecting = 1;
        PyErr_Fetch(&exc, &value, &tb);
        n = collect_with_callback(NUM_GENERATIONS - 1);
        PyErr_Restore(exc, value, tb);
        _PyRuntime.gc.collecting = 0;
    }

    return n;
}

Py_ssize_t
_PyGC_CollectIfEnabled(void)
{
    if (!_PyRuntime.gc.enabled)
        return 0;

    return PyGC_Collect();
}

Py_ssize_t
_PyGC_CollectNoFail(void)
{
    Py_ssize_t n;

    /* Ideally, this function is only called on interpreter shutdown,
       and therefore not recursively.  Unfortunately, when there are daemon
       threads, a daemon thread can start a cyclic garbage collection
       during interpreter shutdown (and then never finish it).
       See http://bugs.python.org/issue8713#msg195178 for an example.
       */
    if (_PyRuntime.gc.collecting)
        n = 0;
    else {
        _PyRuntime.gc.collecting = 1;
        n = collect(NUM_GENERATIONS - 1, NULL, NULL, 1);
        _PyRuntime.gc.collecting = 0;
    }
    return n;
}

void
_PyGC_DumpShutdownStats(void)
{
    if (!(_PyRuntime.gc.debug & DEBUG_SAVEALL)
        && _PyRuntime.gc.garbage != NULL && PyList_GET_SIZE(_PyRuntime.gc.garbage) > 0) {
        const char *message;
        if (_PyRuntime.gc.debug & DEBUG_UNCOLLECTABLE)
            message = "gc: %zd uncollectable objects at " \
                "shutdown";
        else
            message = "gc: %zd uncollectable objects at " \
                "shutdown; use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        /* PyErr_WarnFormat does too many things and we are at shutdown,
           the warnings module's dependencies (e.g. linecache) may be gone
           already. */
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(_PyRuntime.gc.garbage)))
            PyErr_WriteUnraisable(NULL);
        if (_PyRuntime.gc.debug & DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(_PyRuntime.gc.garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr)))
                PyErr_WriteUnraisable(_PyRuntime.gc.garbage);
            else {
                PySys_WriteStderr(
                    "      %s\n",
                    PyBytes_AS_STRING(bytes)
                    );
            }
            Py_XDECREF(repr);
            Py_XDECREF(bytes);
        }
    }
}

void
_PyGC_Fini(void)
{
    Py_CLEAR(_PyRuntime.gc.callbacks);
}

/* for debugging */
void
_PyGC_Dump(PyGC_Head *g)
{
    _PyObject_Dump(FROM_GC(g));
}

/* extension modules might be compiled with GC support so these
   functions must always be available */

#undef PyObject_GC_Track
#undef PyObject_GC_UnTrack
#undef PyObject_GC_Del
#undef _PyObject_GC_Malloc

void
PyObject_GC_Track(void *op)
{
    _PyObject_GC_TRACK(op);
}

void
PyObject_GC_UnTrack(void *op)
{
    /* Obscure:  the Py_TRASHCAN mechanism requires that we be able to
     * call PyObject_GC_UnTrack twice on an object.
     */
    if (IS_TRACKED(op))
        _PyObject_GC_UNTRACK(op);
}

/*
_PyObject_GC_Alloc，用于分配带有垃圾回收头部的 Python 对象内存
参数 use_calloc 是一个整数，用于指示是否使用 calloc 进行内存分配
参数 basicsize 是一个 size_t 类型的值，表示要分配的对象的基本大小
*/
static PyObject *
_PyObject_GC_Alloc(int use_calloc, size_t basicsize)
{
    // 定义一个指向 PyObject 类型的指针 op，用于存储最终分配的 Python 对象
    PyObject *op;
    // 定义一个指向 PyGC_Head 类型的指针 g，用于管理垃圾回收头部信息
    PyGC_Head *g;
    // 定义一个 size_t 类型的变量 size，用于存储要分配的总内存大小
    size_t size;
    
    // 检查 basicsize 是否过大，如果 basicsize 加上 PyGC_Head 的大小超过了 PY_SSIZE_T_MAX
    // 说明可能会导致整数溢出，此时调用 PyErr_NoMemory 函数设置内存不足错误并返回 NULL

    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head))
        return PyErr_NoMemory();

    // 计算要分配的总内存大小，即 PyGC_Head 的大小加上对象的基本大小
    size = sizeof(PyGC_Head) + basicsize;
    // 根据 use_calloc 的值选择不同的内存分配方式
    if (use_calloc)
        // 如果 use_calloc 为真，使用 PyObject_Calloc 函数分配内存
        // 该函数会将分配的内存初始化为 0
        g = (PyGC_Head *)PyObject_Calloc(1, size);
    else
        // 如果 use_calloc 为假，使用 PyObject_Malloc 函数分配内存
        // 该函数分配的内存不会被初始化
        g = (PyGC_Head *)PyObject_Malloc(size);

    // 检查内存分配是否成功，如果 g 为 NULL，说明内存分配失败
    // 调用 PyErr_NoMemory 函数设置内存不足错误并返回 NULL
    
    if (g == NULL)
        return PyErr_NoMemory();
    // 将垃圾回收头部的引用计数初始化为 0
    g->gc.gc_refs = 0;
    // 调用 _PyGCHead_SET_REFS 宏将垃圾回收头部的引用计数标记为 GC_UNTRACKED
    // 表示该对象目前未被垃圾回收机制跟踪

    _PyGCHead_SET_REFS(g, GC_UNTRACKED);
    // 增加第 0 代垃圾回收代的计数
    // 表示新分配了一个带有垃圾回收头部的对象

    _PyRuntime.gc.generations[0].count++; /* number of allocated GC objects */
    // 检查是否满足垃圾回收的条件
    if (_PyRuntime.gc.generations[0].count > _PyRuntime.gc.generations[0].threshold &&
        _PyRuntime.gc.enabled &&
        _PyRuntime.gc.generations[0].threshold &&
        !_PyRuntime.gc.collecting &&
        !PyErr_Occurred()) {
        // 如果满足条件，将 collecting 标志设置为 1，表示正在进行垃圾回收
        _PyRuntime.gc.collecting = 1;
        // 调用 collect_generations 函数进行垃圾回收操作
        collect_generations();
        // 垃圾回收完成后，将 collecting 标志设置为 0，表示垃圾回收结束
        _PyRuntime.gc.collecting = 0;
    }
    // 调用 FROM_GC 宏将 PyGC_Head 指针转换为 PyObject 指针
    op = FROM_GC(g);
    // 返回分配的 Python 对象指针
    return op;
}

PyObject *
_PyObject_GC_Malloc(size_t basicsize)
{
    return _PyObject_GC_Alloc(0, basicsize);
}

PyObject *
_PyObject_GC_Calloc(size_t basicsize)
{
    return _PyObject_GC_Alloc(1, basicsize);
}

// _PyObject_GC_New 函数用于创建一个支持垃圾回收的 Python 对象。
// 参数 tp 是一个指向 PyTypeObject 类型的指针，代表要创建对象的类型。
PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    // 调用 _PyObject_GC_Malloc 函数为对象分配内存。
    // _PyObject_SIZE(tp) 用于计算该类型对象所需的内存大小，它会考虑对象的类型和可能的额外开销。
    // 函数返回一个指向新分配内存的 PyObject 指针。
    PyObject *op = _PyObject_GC_Malloc(_PyObject_SIZE(tp));
    // 检查内存分配是否成功，如果 op 不为 NULL，说明内存分配成功。
    if (op != NULL)
        // 调用 PyObject_INIT 函数对新分配的对象进行初始化。
        // 该函数会将对象的类型设置为 tp 所指向的类型，并进行一些必要的初始化操作。
        // 初始化完成后，返回初始化后的对象指针。
        op = PyObject_INIT(op, tp);
    // 返回创建并初始化好的对象指针，如果内存分配失败则返回 NULL。
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    size_t size;
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *) _PyObject_GC_Malloc(size);
    if (op != NULL)
        op = PyObject_INIT_VAR(op, tp, nitems);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    PyGC_Head *g = AS_GC(op);
    assert(!IS_TRACKED(op));
    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head))
        return (PyVarObject *)PyErr_NoMemory();
    g = (PyGC_Head *)PyObject_REALLOC(g,  sizeof(PyGC_Head) + basicsize);
    if (g == NULL)
        return (PyVarObject *)PyErr_NoMemory();
    op = (PyVarObject *) FROM_GC(g);
    Py_SIZE(op) = nitems;
    return op;
}

void
PyObject_GC_Del(void *op)
{
    PyGC_Head *g = AS_GC(op);
    if (IS_TRACKED(op))
        gc_list_remove(g);
    if (_PyRuntime.gc.generations[0].count > 0) {
        _PyRuntime.gc.generations[0].count--;
    }
    PyObject_FREE(g);
}
