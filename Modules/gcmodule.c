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
�ú�������Ǵ�һ�� Python ����ָ�루PyObject *����ȡ���Ӧ����������ͷ��ָ�루PyGC_Head *����

�� Python ���ڴ沼���У�ÿ��֧���������յĶ�������ʵ�����ݴ洢����֮ǰ����һ�� PyGC_Head �ṹ�壬
���ڴ洢����������ص���Ϣ�������ü�����˫������ָ��ȡ���ˣ�Ҫ��ȡ�������������ͷ����
ֻ��Ҫ������ָ����ǰ�ƶ� PyGC_Head �ṹ���С�ľ��롣

(PyGC_Head *)(o)�����Ƚ�����ָ�� o ǿ��ת��Ϊ PyGC_Head * ���ͣ�
��������Ϊ�˺���ָ�������ܹ����� PyGC_Head �ṹ��Ĵ�С���С�

((PyGC_Head *)(o)-1)����ת�����ָ���ȥ 1����ָ�������У���ȥ 1 
��ζ��ָ����ǰ�ƶ�һ�� PyGC_Head �ṹ���С�ľ��룬�Ӷ��õ��ö������������ͷ��ָ�롣
*/
#define AS_GC(o) ((PyGC_Head *)(o)-1)

/* Get the object given the GC head */
/*
�ú�������Ǵ�һ����������ͷ��ָ�루PyGC_Head *����ȡ���Ӧ�� Python ����ָ�루PyObject *����

���� PyGC_Head �ṹ��λ�� Python ����ʵ�����ݴ洢����֮ǰ������Ҫ����������ͷ��ָ���ȡ����ָ�룬
ֻ��Ҫ����������ͷ��ָ������ƶ� PyGC_Head �ṹ���С�ľ��롣

(PyGC_Head *)g���������ָ�� g ǿ��ת��Ϊ PyGC_Head * ���ͣ�
ȷ������ָ�����㰴�� PyGC_Head �ṹ��Ĵ�С���С�

((PyGC_Head *)g)+1����ת�����ָ����� 1����ָ�������У����� 1 
��ζ��ָ������ƶ�һ�� PyGC_Head �ṹ���С�ľ��룬�Ӷ��õ�����������ͷ����Ӧ�� Python ����ָ�롣

(PyObject *)(((PyGC_Head *)g)+1)����󽫵õ���ָ��ǿ��ת��Ϊ PyObject * ���ͣ��Ա㷵��һ�� Python ����ָ�롣
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
���ã���ʼ������������������ʱ״̬��
������state ָ�� GC ����ʱ״̬�Ľṹ�壬���ڴ洢���� GC ������ú����ݡ�
*/
void
_PyGC_Initialize(struct _gc_runtime_state *state)
{
    // 1 ��ʾ�����Զ���������
    state->enabled = 1; /* automatic collection enabled? */

    // ����һ���� _GEN_HEAD(n)�����ڻ�ȡָ��������n����������������ͷָ��
    // �ú�ͨ������ state �ṹ���� generations ����ĵ� n ��Ԫ�ص� head ��Ա��ʵ��
#define _GEN_HEAD(n) (&state->generations[n].head)
    // ����һ���ֲ����� generations�����ڳ�ʼ����ͬ����������������Ϣ
    // ����Ĵ�С�� NUM_GENERATIONS �궨�壬�����������յĴ�������
    struct gc_generation generations[NUM_GENERATIONS] = {
        // ÿһ������������Ϣ����һ�� PyGC_Head �ṹ�塢һ����ֵ��һ������
        // PyGC_Head �ṹ�����ڹ���˫��������ֵ���ھ�����ʱ�����������գ��������ڼ�¼��ǰ�����ж��������
        /* PyGC_Head,                                 threshold,      count */
        // �� 0 ������������Ϣ��ʼ��
        // ˫�������ͷָ���βָ�붼ָ��������ֵΪ 700����ʼ����Ϊ 0
        {{{_GEN_HEAD(0), _GEN_HEAD(0), 0}},           700,            0},
        // �� 1 ������������Ϣ��ʼ��
        // ˫�������ͷָ���βָ�붼ָ��������ֵΪ 10����ʼ����Ϊ 0
        {{{_GEN_HEAD(1), _GEN_HEAD(1), 0}},           10,             0},
        // �� 2 ������������Ϣ��ʼ��
        // ˫�������ͷָ���βָ�붼ָ��������ֵΪ 10����ʼ����Ϊ 0
        {{{_GEN_HEAD(2), _GEN_HEAD(2), 0}},           10,             0},
    };
    // ���� generations ���飬���ֲ������е���Ϣ���Ƶ� state �ṹ��� generations ������
    for (int i = 0; i < NUM_GENERATIONS; i++) {
        state->generations[i] = generations[i];
    };
    // ���� 0 ���������������ͷָ�븳ֵ�� state �ṹ��� generation0 ��Ա
    // ����������ٷ��ʵ� 0 ��������������
    state->generation0 = GEN_HEAD(0);
    // ����һ�����ô�������������Ϣ�ṹ�� permanent_generation
    // ���ô���˫�������ͷָ���βָ�붼ָ��������ֵΪ 0����ʼ����Ϊ 0
    struct gc_generation permanent_generation = {
          {{&state->permanent_generation.head, &state->permanent_generation.head, 0}}, 0, 0
    };
    // �����ô�������������Ϣ��ֵ�� state �ṹ��� permanent_generation ��Ա
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
 * gc_list_init �������ڳ�ʼ��һ������ PyGC_Head �ṹ���˫������
 * �� Python ���������ջ����У���ʹ��˫������������ɱ��������յĶ���
 * �ú����������ǽ������������ʼ��Ϊ������״̬��
 *
 * @param list ָ�� PyGC_Head �ṹ���ָ�룬����Ҫ��ʼ��������ͷ�ڵ㡣
 */
static void
gc_list_init(PyGC_Head* list)
{
    // ������ͷ�ڵ�� gc_prev ָ��ָ������
    // ��˫�������У�gc_prev ָ��ͨ������ָ��ǰһ���ڵ㣬
    // ������Ϊ��ʱ��ͷ�ڵ��ǰһ���ڵ�������Լ���
    list->gc.gc_prev = list;

    // ������ͷ�ڵ�� gc_next ָ��ָ������
    // ͬ��gc_next ָ������ָ���һ���ڵ㣬
    // �������ͷ�ڵ�ĺ�һ���ڵ�Ҳ�����Լ���
    list->gc.gc_next = list;
}
/**
 * gc_list_is_empty ���������ж�һ������ PyGC_Head �ṹ�幹����˫�������Ƿ�Ϊ�ա�
 * �� Python ���������ջ����У���ʹ��˫������������ɱ��������յĶ���
 * �ú���Ϊ�ж������Ƿ�Ϊ���ṩ��һ�ּ򵥵ķ�ʽ��
 *
 * @param list ָ�� PyGC_Head �ṹ���ָ�룬����Ҫ���������ͷ�ڵ㡣
 * @return �������Ϊ�գ����ط���ֵ��ͨ��Ϊ 1�����������Ϊ�գ����� 0��
 */
static int
gc_list_is_empty(PyGC_Head* list)
{
    // ��˫�������У��������Ϊ�գ�ͷ�ڵ�� gc_next ָ���ָ������
    // ��Ϊ�ڿ������У�û�������ڵ㣬����ͷ�ڵ����һ���ڵ�������Լ���
    // ����ͨ���Ƚ�ͷ�ڵ�� gc_next ָ���ͷ�ڵ㱾���Ƿ�������ж������Ƿ�Ϊ�ա�
    // �����ȣ�˵������Ϊ�գ����ط���ֵ������˵������Ϊ�գ����� 0��
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
 * gc_list_merge �������ڽ�һ���������ն�������`from`���ϲ�����һ������`to`���С�
 * �����������ǻ��� `PyGC_Head` �ṹ�幹����˫������������ Python ���������ջ��ơ�
 *
 * @param from ָ��Ҫ�ϲ���Դ����� `PyGC_Head` �ṹ��ָ�롣�ϲ���ɺ󣬸�����ᱻ��ա�
 * @param to ָ��Ŀ������� `PyGC_Head` �ṹ��ָ�룬Դ�����Ԫ�ؽ�����ӵ���������С�
 */
static void
gc_list_merge(PyGC_Head* from, PyGC_Head* to)
{
    // ����һ��ָ�� `PyGC_Head` �ṹ���ָ�� `tail`��������ʱ�洢Ŀ�������β���ڵ㡣
    PyGC_Head* tail;

    // ʹ�� `assert` ��ȷ�� `from` �� `to` ����ͬһ������
    // ��� `from` �� `to` ��ͬ���ϲ�����û�����壬���ҿ��ܵ�������ṹ�𻵣����������ж��Լ�顣
    assert(from != to);

    // ���Դ���� `from` �Ƿ�Ϊ�ա�
    // `gc_list_is_empty` ���������ж������Ƿ�Ϊ�գ��������Ϊ�գ���ִ�кϲ�������
    if (!gc_list_is_empty(from)) {
        // ��ȡĿ������ `to` ��β���ڵ㣬���丳ֵ�� `tail` ָ�롣
        tail = to->gc.gc_prev;

        // �������д����������ڵ�����Ӳ�������Դ���� `from` ���뵽Ŀ������ `to` ��β����

        // ��Ŀ������β���ڵ� `tail` �� `gc_next` ָ��ָ��Դ����ĵ�һ����Ч�ڵ㣨`from->gc.gc_next`����
        tail->gc.gc_next = from->gc.gc_next;
        // ��Դ�����һ����Ч�ڵ�� `gc_prev` ָ��ָ��Ŀ�������β���ڵ� `tail`��
        tail->gc.gc_next->gc.gc_prev = tail;
        // ��Ŀ������ `to` �� `gc_prev` ָ��ָ��Դ��������һ����Ч�ڵ㣨`from->gc.gc_prev`����
        to->gc.gc_prev = from->gc.gc_prev;
        // ��Դ�������һ����Ч�ڵ�� `gc_next` ָ��ָ��Ŀ������ `to`��
        to->gc.gc_prev->gc.gc_next = to;
    }

    // �ϲ���ɺ󣬽�Դ���� `from` ��ʼ��Ϊ������
    // `gc_list_init` �������ڽ������ʼ��Ϊ��״̬��ͨ���Ὣ�����ͷ�ڵ��β�ڵ�ָ������
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
 // ��������˵���������ж���� gc_refs �ֶ�����Ϊ�����ü��� ob_refcnt��
 // ������ɺ����������ڶ���� gc_refs ���� 0������δ���������ұ��������ջ��Ƹ��ٵĶ���� gc_refs Ϊ GC_REACHABLE
static void
update_refs(PyGC_Head* containers)
{
    // �� containers ָ�����һ������ʼ����
    PyGC_Head* gc = containers->gc.gc_next;
    // ѭ���������б��������ջ��Ƹ��ٵĶ���ֱ���ص� containers ����
    for (; gc != containers; gc = gc->gc.gc_next) {
        // ���Ե�ǰ����� gc_refs Ϊ GC_REACHABLE
        // ����Ϊ��ȷ���ڸ���֮ǰ�������״̬�Ƿ���Ԥ�ڵ�
        assert(_PyGCHead_REFS(gc) == GC_REACHABLE);
        // ����ǰ����� gc_refs ����Ϊ�����ü���
        // FROM_GC(gc) ���ڴ� PyGC_Head ָ���ȡ��Ӧ�� Python ����ָ��
        // Py_REFCNT ���ڻ�ȡ�ö�������ü���
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
         // ע��˵����Python ��ѭ���������ջ��Ʋ�Ӧ�ÿ������ü���Ϊ 0 �Ķ���
         // ���һ����������ü�����Ϊ 0����Ӧ���������ͷš�
         // ������Դ��������ܵ�ԭ���ǣ��ڶ��������������tp_dealloc���У�һ��֧���������յĶ���
         // �������ٽ׶��Ա����٣���������һЩ������������һЩ�����ص��� Python �У�
         // ��ʱ�������ջ��ƿ��ܻᴥ�������ҿ������ڱ����ٵļ������ٵĶ���
         // ������������֮ǰ�������Ĵ���ᵼ���������ջ��Ƴ����ٴ�ɾ���ö���
         // �ڵ��԰汾�У��� _Py_ForgetReference ���Եڶ��δ����ж����˫���������Ƴ��ö���ʱ��
         // �ᵼ�����صĶδ����ڷ����汾�У��ᷢ��ʵ�ʵ�˫���ͷţ���ᵼ���ڴ���������ڲ�����ָ���𻵡�
         // ��������ǳ����أ�Ҳ��������Ӧ���ڷ����汾�н��У�������������Ϊһ�����ԡ�
        assert(_PyGCHead_REFS(gc) != 0);
    }
}

/* A traversal callback for subtract_refs. */
// ��������һ������ subtract_refs �ı����ص��������� Python ���������ջ����У�
// ������ʹ�ûص���������������ͼ����ÿ������ִ���ض��Ĳ���������Ĳ������Ǽ������ü�����
static int
visit_decref(PyObject* op, void* data)
{
    // ���� op ��Ϊ��ָ�롣����һ�ַ����Ա�̵��ֶΣ�ȷ������Ķ���ָ������Ч�ġ�
    assert(op != NULL);
    // ������ op �Ƿ���֧���������յĶ����� Python �У�ֻ�в��ֶ�������֧���������գ�
    // ��Щ��������һ�� PyGC_Head �ṹ������������������ص���Ϣ��
    if (PyObject_IS_GC(op)) {
        // �������֧���������գ�������ָ��ת��Ϊ PyGC_Head ָ�롣
        // PyGC_Head �� Python �����ڹ����������յ�ͷ���ṹ�������˶�������ü�������Ϣ��
        PyGC_Head* gc = AS_GC(op);
        /* We're only interested in gc_refs for objects in the
         * generation being collected, which can be recognized
         * because only they have positive gc_refs.
         */
         // ע�ͽ���������ֻ��ע���ڱ����յĴ���generation���еĶ���� gc_refs��
         // �� Python �ķִ��������ջ����У����󱻷�Ϊ��ͬ�Ĵ���ÿһ���в�ͬ�Ļ���Ƶ�ʡ�
         // ֻ�����ڱ����յĴ��еĶ���� gc_refs ����������������� gc_refs ����Ϊ������ 0��
         // ���� gc_refs ��Ϊ 0�����Ϊ 0 ˵����������ü�����С�����ܴ��ڴ���
        assert(_PyGCHead_REFS(gc) != 0); /* else refcount was too small */
        // ������� gc_refs �Ƿ�Ϊ���������Ϊ������˵���ö��������ڱ����յĴ��еĶ���
        if (_PyGCHead_REFS(gc) > 0)
            // �������� gc_refs Ϊ���������� _PyGCHead_DECREF �����������ü����� 1��
            // �����������չ����е�һ����Ҫ���裬ͨ���������ü�������Ƕ����Ƿ���Ա����ա�
            _PyGCHead_DECREF(gc);
    }
    // �ص��������� 0 ��ʾ������������ Python �ı��������У��ص��������� 0 ��ʾ��������
    // ���Լ���������һ�����󣻷��ط� 0 ֵ��ʾ���ִ������Ҫֹͣ������
    return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
 // ����һ����̬���� subtract_refs��������һ��ָ�� PyGC_Head �ṹ���ָ�� containers ��Ϊ����
 // ����������ڼ�ȥ����֮���ѭ�����ü���
static void
subtract_refs(PyGC_Head* containers)
{
    // ����һ�� traverseproc ���͵ı��� traverse
    // traverseproc ��һ������ָ�����ͣ�����ָ������ tp_traverse ����
    traverseproc traverse;
    // �� containers ָ�����һ������ʼ����
    // ��Ϊ containers ������һ���ڱ��ڵ㣬������Ҫ����Ķ����������һ���ڵ㿪ʼ
    PyGC_Head* gc = containers->gc.gc_next;
    // ѭ���������б��������ջ��Ƹ��ٵĶ���ֱ���ص� containers ����
    for (; gc != containers; gc = gc->gc.gc_next) {
        // ��ȡ��ǰ��������Ͷ��󣬲�������ȡ tp_traverse ����
        // Py_TYPE(FROM_GC(gc)) ���ڻ�ȡ��ǰ PyGC_Head ��Ӧ�� Python ��������Ͷ���
        // tp_traverse �����Ͷ����е�һ������ָ�룬���ڱ������������
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        // ���õ�ǰ����� tp_traverse ����
        // FROM_GC(gc) �ǵ�ǰ�����ָ��
        // (visitproc)visit_decref ��һ�����ʺ����������ڱ��������м�ȥ���ü���
        // NULL �Ǵ��ݸ����ʺ����Ķ������������û��ʹ��
        // (void) ǿ������ת����Ϊ�˱������������δʹ�÷���ֵ�ľ���
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
 // ����һ����̬���� move_unreachable������������ָ�� PyGC_Head �ṹ���ָ�� young �� unreachable ��Ϊ����
 // young �����������Ҫ�����������ռ��Ķ���unreachable �������ڴ�ſ��ܲ��ɴ�Ķ���
static void
move_unreachable(PyGC_Head* young, PyGC_Head* unreachable)
{
    // �� young ����ĵ�һ������ʼ����
    PyGC_Head* gc = young->gc.gc_next;

    /* Invariants:  all objects "to the left" of us in young have gc_refs
     * = GC_REACHABLE, and are indeed reachable (directly or indirectly)
     * from outside the young list as it was at entry.  All other objects
     * from the original young "to the left" of us are in unreachable now,
     * and have gc_refs = GC_TENTATIVELY_UNREACHABLE.  All objects to the
     * left of us in 'young' now have been scanned, and no objects here
     * or to the right have been scanned yet.
     */
     // ����ʽ˵����
     // �� young �����У���ǰ����λ�á���ࡱ�����ж���� gc_refs Ϊ GC_REACHABLE��
     // ������Щ����ȷʵ�ӽ��뺯��ʱ young �����ⲿ��ֱ�ӻ��ӣ��ɴ
     // ԭ young �����е�ǰ����λ�á���ࡱ�������������ڶ��� unreachable �����У�
     // �������ǵ� gc_refs Ϊ GC_TENTATIVELY_UNREACHABLE��
     // young �����е�ǰ����λ�á���ࡱ�����ж����Ѿ���ɨ���������ǰλ�ü��䡰�Ҳࡱ�Ķ���δ��ɨ�衣

     // ��ʼ���� young ����ֱ���ص�����ͷ��young��
    while (gc != young) {
        // ���浱ǰ�������һ������ָ�룬��Ϊ�ڴ�������е�ǰ�����λ�ÿ��ܻ�ı�
        PyGC_Head* next;

        // ��鵱ǰ����� gc_refs �Ƿ�Ϊ 0
        if (_PyGCHead_REFS(gc)) {
            /* gc is definitely reachable from outside the
             * original 'young'.  Mark it as such, and traverse
             * its pointers to find any other objects that may
             * be directly reachable from it.  Note that the
             * call to tp_traverse may append objects to young,
             * so we have to wait until it returns to determine
             * the next object to visit.
             */
             // ˵������ǰ����϶���ԭ young �����ⲿ�ɴ
             // �����Ϊ�ɴ�������������ã����ҳ���������ֱ�Ӵ����ɴ�Ķ���
             // ע�⣺���� tp_traverse ���ܻ��� young ����׷�Ӷ����������Ǳ���������غ���ȷ����һ��Ҫ���ʵĶ���
             // ��ȡ��ǰ PyGC_Head ��Ӧ�� Python ����ָ��
            PyObject* op = FROM_GC(gc);
            // ��ȡ��ǰ�������͵� tp_traverse ���������ڱ������������
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            // ���Ե�ǰ����� gc_refs ���� 0��ȷ������ɴ�
            assert(_PyGCHead_REFS(gc) > 0);
            // ����ǰ����� gc_refs ���Ϊ GC_REACHABLE����ʾ���ǿɴ��
            _PyGCHead_SET_REFS(gc, GC_REACHABLE);
            // ���� tp_traverse ���������� visit_reachable ��Ϊ���ʺ�����young ��Ϊ�������
            // �÷����������ǰ��������ã������ɴ������Ϊ�ɴ�
            (void)traverse(op,
                (visitproc)visit_reachable,
                (void*)young);
            // ��ȡ��ǰ�������һ������ָ��
            next = gc->gc.gc_next;
            // �����ǰ������Ԫ�����ͣ�����ȡ�������ĸ���
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
             // ˵������ǰ������ܲ��ɴΪ�˼����������������ɴ
             // ��ǰ�����ܴ������Ѿ����������κζ���ֱ�ӿɴ�����ܴ����ǻ�δ����Ķ���ɴ
             // �����������visit_reachable ���ջὫ��ǰ�����ƻ� young �������ǻ��ٴδ�������
             // ��ȡ��ǰ�������һ������ָ��
            next = gc->gc.gc_next;
            // ����ǰ����� young �����ƶ��� unreachable ����
            gc_list_move(gc, unreachable);
            // ����ǰ����� gc_refs ���Ϊ GC_TENTATIVELY_UNREACHABLE����ʾ�����ܲ��ɴ�
            _PyGCHead_SET_REFS(gc, GC_TENTATIVELY_UNREACHABLE);
        }
        // �ƶ�����һ�������������
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
 // ����һ����̬���� move_legacy_finalizers������������ָ�� PyGC_Head �ṹ���ָ��
 // unreachable ���������֮ǰ�����Ϊ���ܲ��ɴ�Ķ���
 // finalizers �������ڴ�ž��оɰ��ս����Ķ���
static void
move_legacy_finalizers(PyGC_Head* unreachable, PyGC_Head* finalizers)
{
    // ��������ָ�� PyGC_Head �ṹ���ָ��
    // gc ���ڱ��� unreachable �����еĶ���
    // next ���ڱ��浱ǰ�������һ��������Ϊ���ƶ�����ʱ��ǰ����������ϵ��ı�
    PyGC_Head* gc;
    PyGC_Head* next;

    /* March over unreachable.  Move objects with finalizers into
     * `finalizers`.
     */
     // ˵�������� unreachable �����������ս����Ķ����ƶ��� finalizers ������

     // �� unreachable ����ĵ�һ������ʼ������ֱ���ص�����ͷ��unreachable��
    for (gc = unreachable->gc.gc_next; gc != unreachable; gc = next) {
        // ��ȡ��ǰ PyGC_Head ��Ӧ�� Python ����ָ��
        PyObject* op = FROM_GC(gc);

        // ���Ե�ǰ�����ڿ��ܲ��ɴ�״̬
        // ȷ����ǰ����ȷʵ��֮ǰ���Ϊ���ܲ��ɴ�Ķ���
        assert(IS_TENTATIVELY_UNREACHABLE(op));

        // ���浱ǰ�������һ������ָ��
        // ��Ϊ�������ܻὫ��ǰ����� unreachable �������Ƴ���������Ҫ��ǰ������һ������
        next = gc->gc.gc_next;

        // ��鵱ǰ�����Ƿ���оɰ��ս���
        if (has_legacy_finalizer(op)) {
            // ������оɰ��ս���������ǰ����� unreachable �����ƶ��� finalizers ����
            gc_list_move(gc, finalizers);
            // ���ö�������ü���״̬���Ϊ�ɴ�
            // ��ΪҪ����Щ���ս����Ķ�����е���������ʱ��Ϊ�����ǿɴ��
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
 // ����һ����̬���� move_legacy_finalizer_reachable��������һ��ָ�� PyGC_Head �ṹ���ָ�� finalizers ��Ϊ����
 // finalizers �������ž��оɰ��ս����Ķ���
static void
move_legacy_finalizer_reachable(PyGC_Head* finalizers)
{
    // ����һ�� traverseproc ���͵ı��� traverse
    // traverseproc ��һ������ָ�����ͣ�����ָ������ tp_traverse �������÷������ڱ������������
    traverseproc traverse;
    // �� finalizers ����ĵ�һ������ʼ����
    // ��Ϊ finalizers ����ɿ��������ͷ�ڵ㣬����Ҫ����Ķ����������һ���ڵ㿪ʼ
    PyGC_Head* gc = finalizers->gc.gc_next;
    // ѭ������ finalizers �����е����ж���ֱ���ص�����ͷ��finalizers��
    for (; gc != finalizers; gc = gc->gc.gc_next) {
        /* Note that the finalizers list may grow during this. */
        // ע��˵�����ڱ��������У�finalizers ������ܻ�����
        // ������Ϊ�ڵ��� tp_traverse ������ʹ�� visit_move ���ʺ���ʱ��
        // ���ܻᷢ���µĿɴ���󲢽�������ӵ� finalizers ������

        // ��ȡ��ǰ��������Ͷ��󣬲�������ȡ tp_traverse ����
        // Py_TYPE(FROM_GC(gc)) ���ڻ�ȡ��ǰ PyGC_Head ��Ӧ�� Python ��������Ͷ���
        // tp_traverse �����Ͷ����е�һ������ָ�룬���ڱ������������
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        // ���õ�ǰ����� tp_traverse ����
        // FROM_GC(gc) �ǵ�ǰ�����ָ��
        // (visitproc)visit_move ��һ�����ʺ����������ڱ��������д���ɴ���󣬿��ܻὫ�ɴ�����ƶ��� finalizers ����
        // (void *)finalizers �Ǵ��ݸ����ʺ����Ķ�����������ﴫ�� finalizers ����ָ�룬������ʺ�������
        // (void) ǿ������ת����Ϊ�˱������������δʹ�÷���ֵ�ľ���
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
 // ����һ����̬���� handle_legacy_finalizers����������ָ�� PyGC_Head �ṹ���ָ��
 // finalizers ����������оɰ��ս����Ķ���
 // old �������ںϲ� finalizers �����еĶ���
static void
handle_legacy_finalizers(PyGC_Head* finalizers, PyGC_Head* old)
{
    // �� finalizers ����ĵ�һ������ʼ����
    PyGC_Head* gc = finalizers->gc.gc_next;

    // ��� _PyRuntime.gc.garbage �б��Ƿ�Ϊ��
    if (_PyRuntime.gc.garbage == NULL) {
        // ���Ϊ�գ�����һ���µĿ��б�
        _PyRuntime.gc.garbage = PyList_New(0);
        // ����б��Ƿ񴴽��ɹ�
        if (_PyRuntime.gc.garbage == NULL)
            // �������ʧ�ܣ�������������
            Py_FatalError("gc couldn't create gc.garbage list");
    }

    // ���� finalizers �����е����ж���
    for (; gc != finalizers; gc = gc->gc.gc_next) {
        // ��ȡ��ǰ PyGC_Head ��Ӧ�� Python ����ָ��
        PyObject* op = FROM_GC(gc);

        // ����Ƿ����� DEBUG_SAVEALL ����ģʽ���߶����Ƿ���оɰ��ս���
        if ((_PyRuntime.gc.debug & DEBUG_SAVEALL) || has_legacy_finalizer(op)) {
            // �����������������ǰ������ӵ� _PyRuntime.gc.garbage �б���
            if (PyList_Append(_PyRuntime.gc.garbage, op) < 0)
                // ������ʧ�ܣ�����ѭ��
                break;
        }
    }

    // �� finalizers ����ϲ��� old ������
    gc_list_merge(finalizers, old);
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
 // ����һ����̬���� finalize_garbage��������һ��ָ�� PyGC_Head �ṹ���ָ�� collectable ��Ϊ����
 // collectable ��������˴�ִ���ս�����Ķ���
static void
finalize_garbage(PyGC_Head* collectable)
{
    // ����һ�� destructor ���͵ı��� finalize
    // destructor ��һ������ָ�����ͣ�����ָ������ tp_finalize �������÷�������ִ�ж�����ս����
    destructor finalize;
    // ����һ�� PyGC_Head ���͵ı��� seen
    // �������������ʱ��Ŵ� collectable ������ȡ���Ķ����Ա������ս��������������ṹ�仯����������
    PyGC_Head seen;

    /* While we're going through the loop, `finalize(op)` may cause op, or
     * other objects, to be reclaimed via refcounts falling to zero.  So
     * there's little we can rely on about the structure of the input
     * `collectable` list across iterations.  For safety, we always take the
     * first object in that list and move it to a temporary `seen` list.
     * If objects vanish from the `collectable` and `seen` lists we don't
     * care.
     */
     // ע��˵������ѭ��ִ�й����У����� `finalize(op)` ���ܻᵼ�¶��� `op` ������������Ϊ���ü�����Ϊ��������ա�
     // ������ÿ�ε����У�`collectable` ����Ľṹ���ܻᷢ���ܴ�仯������������ԭ���Ľṹ��
     // Ϊ�˱�֤��ȫ�ԣ���������ȡ�� `collectable` ����ĵ�һ�����󣬲������ƶ�����ʱ�� `seen` �����С�
     // �������� `collectable` �� `seen` ��������ʧ�����ǲ���Ҫ���ġ�

     // ��ʼ�� seen ����
     // ȷ�� seen �����ָ�����Ϣ��ȷ���ã�Ϊ�����洢������׼��
    gc_list_init(&seen);

    // �� collectable ����Ϊ��ʱ������ѭ��
    while (!gc_list_is_empty(collectable)) {
        // ��ȡ collectable ����ĵ�һ������
        PyGC_Head* gc = collectable->gc.gc_next;
        // ��ȡ��ǰ PyGC_Head ��Ӧ�� Python ����ָ��
        PyObject* op = FROM_GC(gc);
        // ����ǰ����� collectable �����ƶ��� seen ����
        // ������ִ���ս����ʱ collectable ����ṹ����
        gc_list_move(gc, &seen);

        // ��鵱ǰ�����Ƿ�����ִ���ս����������
        // ���� 1������δ���ս������ _PyGCHead_FINALIZED(gc) Ϊ 0
        // ���� 2����������;����ս������ԣ��� PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_HAVE_FINALIZE) Ϊ��
        // ���� 3����������Ͷ����� tp_finalize �������� Py_TYPE(op)->tp_finalize ��Ϊ NULL
        if (!_PyGCHead_FINALIZED(gc) &&
            PyType_HasFeature(Py_TYPE(op), Py_TPFLAGS_HAVE_FINALIZE) &&
            (finalize = Py_TYPE(op)->tp_finalize) != NULL) {
            // ��ǵ�ǰ�����Ѿ����ս�
            _PyGCHead_SET_FINALIZED(gc, 1);
            // ���Ӷ�������ü���
            // ��ֹ��ִ���ս���������ж�����Ϊ���ü�����Ϊ�������ǰ����
            Py_INCREF(op);
            // ���ö�����ս�������
            finalize(op);
            // ���ٶ�������ü���
            // �ָ�����ԭ�������ü���״̬
            Py_DECREF(op);
        }
    }

    // �� seen �����еĶ���ϲ��� collectable ����
    // ����ս�����󣬻ָ� collectable �����������
    gc_list_merge(&seen, collectable);
}

/* Walk the collectable list and check that they are really unreachable
   from the outside (some objects could have been resurrected by a
   finalizer). */
   // ����һ����̬���� check_garbage��������һ��ָ�� PyGC_Head �ṹ���ָ�� collectable ��Ϊ����
   // collectable ��������˴�����Ƿ�Ϊ��������Ķ���
static int
check_garbage(PyGC_Head* collectable)
{
    // ����һ��ָ�� PyGC_Head �ṹ���ָ�� gc�����ڱ��� collectable ����
    PyGC_Head* gc;

    // ��һ��ѭ�������¶�������ü���
    // �� collectable ����ĵ�һ������ʼ������ֱ���ص�����ͷ��collectable��
    for (gc = collectable->gc.gc_next; gc != collectable;
        gc = gc->gc.gc_next) {
        // ����ǰ����� gc_refs �ֶ�����Ϊ�ö����ʵ�����ü���
        // FROM_GC(gc) ���ڴ� PyGC_Head ָ���ȡ��Ӧ�� Python ����ָ��
        // Py_REFCNT ���ڻ�ȡ�ö�������ü���
        _PyGCHead_SET_REFS(gc, Py_REFCNT(FROM_GC(gc)));
        // ���Ե�ǰ��������ü�����Ϊ 0
        // ȷ�������ڸ������ü����������ü�������Ч��
        assert(_PyGCHead_REFS(gc) != 0);
    }

    // ���� subtract_refs ��������ȥ����֮���ѭ�����ü���
    // �ú�������� collectable �����е�ÿ�����󣬵����� tp_traverse ������
    // ��ʹ�� visit_decref ��Ϊ���ʺ������Ӷ���ȥѭ�����ü���
    subtract_refs(collectable);

    // �ڶ���ѭ�������ʣ�����ü���
    // �ٴδ� collectable ����ĵ�һ������ʼ������ֱ���ص�����ͷ��collectable��
    for (gc = collectable->gc.gc_next; gc != collectable;
        gc = gc->gc.gc_next) {
        // ���Ե�ǰ�����ʣ�����ü������ڵ��� 0
        // ȷ����ȥѭ�����ü��������ü�������Ϊ����
        assert(_PyGCHead_REFS(gc) >= 0);
        // �����ǰ�����ʣ�����ü�����Ϊ 0��˵���ö������ⲿ���ã�������������
        if (_PyGCHead_REFS(gc) != 0)
            // ���� -1 ��ʾ���ʧ�ܣ����ڷ���������
            return -1;
    }

    // ������ж����ʣ�����ü�����Ϊ 0��˵�� collectable �����еĶ�������������
    // ���� 0 ��ʾ���ɹ�
    return 0;
}

// ����һ����̬���� revive_garbage��������һ��ָ�� PyGC_Head �ṹ���ָ�� collectable ��Ϊ����
// collectable �����������Щ���ܱ�������Ϊ���ɴ�Ķ�����Ҫ�����ǻָ�Ϊ�ɴ�״̬
static void
revive_garbage(PyGC_Head* collectable)
{
    // ����һ��ָ�� PyGC_Head �ṹ���ָ�� gc�����ڱ��� collectable ����
    PyGC_Head* gc;

    // ���� collectable ����������ĵ�һ������ʼ��ֱ���ص�����ͷ��collectable��
    for (gc = collectable->gc.gc_next; gc != collectable;
        gc = gc->gc.gc_next) {
        // ����ǰ����� gc_refs �ֶ�����Ϊ GC_REACHABLE����ʾ�ö����ǿɴ��
        // ��������Ϊ�˳���֮ǰ���ܵĲ��ɴ��ǣ��ָ����������״̬
        _PyGCHead_SET_REFS(gc, GC_REACHABLE);
    }
}

/* Break reference cycles by clearing the containers involved.  This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
 // ����һ����̬���� delete_garbage������������ָ�� PyGC_Head �ṹ���ָ�� collectable �� old ��Ϊ����
 // collectable ��������˴��������������
 // old �������ڴ����Щ�������������Ȼ���Ķ���
static void
delete_garbage(PyGC_Head* collectable, PyGC_Head* old)
{
    // ����һ�� inquiry ���͵ı��� clear
    // inquiry ��һ������ָ�����ͣ�����ָ������ tp_clear �������÷�������������������
    inquiry clear;

    // �� collectable ����Ϊ��ʱ������ѭ���������еĶ���
    while (!gc_list_is_empty(collectable)) {
        // ��ȡ collectable ����ĵ�һ������
        PyGC_Head* gc = collectable->gc.gc_next;
        // ��ȡ��ǰ PyGC_Head ��Ӧ�� Python ����ָ��
        PyObject* op = FROM_GC(gc);

        // ����Ƿ����� DEBUG_SAVEALL ����ģʽ
        if (_PyRuntime.gc.debug & DEBUG_SAVEALL) {
            // ��������� DEBUG_SAVEALL ����ģʽ������ǰ������ӵ� _PyRuntime.gc.garbage �б���
            // ��������Ϊ���ڵ���ʱ������Щ�������󣬷����������
            PyList_Append(_PyRuntime.gc.garbage, op);
        }
        else {
            // ���û�п��� DEBUG_SAVEALL ����ģʽ������������������
            // ��ȡ�������͵� tp_clear ����
            if ((clear = Py_TYPE(op)->tp_clear) != NULL) {
                // ���Ӷ�������ü�������ֹ��������������ж�����ǰ����
                Py_INCREF(op);
                // ���ö���� tp_clear ������������������
                clear(op);
                // ���ٶ�������ü������ָ�����ԭ�������ü���״̬
                Py_DECREF(op);
            }
        }

        // �������Ƿ���Ȼ���
        // ��� collectable �������һ��������Ȼ�ǵ�ǰ����˵�������������������Ȼ���
        if (collectable->gc.gc_next == gc) {
            // ע��˵����������Ȼ�������ƶ��� old �����У��������ں��������б�����
            // ����ǰ����� collectable �����ƶ��� old ����
            gc_list_move(gc, old);
            // ���ö�������ü���״̬���Ϊ�ɴ�
            // ��Ϊ������Ȼ��������ʱ��Ϊ���ǿɴ��
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
  * collect ������ Python �������ջ��Ƶĺ��ĺ���������ִ��ָ���������������ղ�����
  * �����ǲ�������Щ���ɴ�Ķ���ͬʱ��������ս����Ķ��󣬱����ڴ�й©��
  *
  * @param generation Ҫ�����������յĴ���������Խ�ߣ�������ʱ��Խ��������Ƶ��Խ�͡�
  * @param n_collected ָ�� Py_ssize_t ���͵�ָ�룬���ڴ洢�ɹ����յĶ���������
  * @param n_uncollectable ָ�� Py_ssize_t ���͵�ָ�룬���ڴ洢�޷����յĶ���������
  * @param nofail һ����־��ָʾ���������չ����з�������ʱ�Ƿ���Դ���
  * @return ���سɹ����պ��޷����յĶ���������
    ׼����������¼������Ϣ�����¼��������ϲ�����������Ķ�������
    ��ǿɴ����ͨ�����ºͼ�ȥ���ü�������ǳ����ⲿ�ɴ�Ķ���
    ���벻�ɴ���󣺽����ɴ�����ƶ��� unreachable ����
    ����ɴ���󣺽��ɴ�����ƶ�����һ����������⴦��
    �����ս�������������оɰ��ս����Ĳ��ɴ����
    �������������������á������ս�������������ѭ����ɾ����������
    ͳ�ƺ͵��ԣ�ͳ�Ʋ����ռ��������������������Ϣ��
    �쳣���������������չ����з������쳣��
    ����ͳ����Ϣ�����µ�ǰ�������ռ�ͳ����Ϣ��
    �������������� DTrace ̽�룬���ػ��պͲ����ռ�����������
  */
static Py_ssize_t
collect(int generation, Py_ssize_t* n_collected, Py_ssize_t* n_uncollectable,
    int nofail)
{
    int i;
    // m ���ڼ�¼�ɹ����յĶ�������
    Py_ssize_t m = 0;
    // n ���ڼ�¼�޷����յĶ�������
    Py_ssize_t n = 0;
    // young ָ��ǰҪ���Ĵ����Ķ�������ͷ
    PyGC_Head* young;
    // old ָ����һ�����ϴ����Ķ�������ͷ
    PyGC_Head* old;
    // unreachable ���ڴ洢���ɴ������������������
    PyGC_Head unreachable;
    // finalizers ���ڴ洢���� __del__ �����Լ�����Щ����ɴ�Ķ���
    PyGC_Head finalizers;
    PyGC_Head* gc;
    // t1 ���ڼ�¼�������տ�ʼʱ�䣬��������������ȳ�ʼ��Ϊ 0
    _PyTime_t t1 = 0;

    // ��ȡ��ǰ������ͳ����Ϣ�ṹ��ָ��
    struct gc_generation_stats* stats = &_PyRuntime.gc.generation_stats[generation];

    // ��������� DEBUG_STATS ����ģʽ������������տ�ʼ��Ϣ�͸�����������
    if (_PyRuntime.gc.debug & DEBUG_STATS) {
        PySys_WriteStderr("gc: collecting generation %d...\n",
            generation);
        PySys_WriteStderr("gc: objects in each generation:");
        for (i = 0; i < NUM_GENERATIONS; i++)
            PySys_FormatStderr(" %zd",
                gc_list_size(GEN_HEAD(i)));
        PySys_WriteStderr("\ngc: objects in permanent generation: %zd",
            gc_list_size(&_PyRuntime.gc.permanent_generation.head));
        // ��¼�������տ�ʼʱ��
        t1 = _PyTime_GetMonotonicClock();

        PySys_WriteStderr("\n");
    }

    // ��� DTrace ������ GC_START ̽�룬������̽��
    if (PyDTrace_GC_START_ENABLED())
        PyDTrace_GC_START(generation);

    /* �����ռ��ͷ�������� */
    // �����ǰ����������ߴ���������һ�����ϴ����ļ����� 1
    if (generation + 1 < NUM_GENERATIONS)
        _PyRuntime.gc.generations[generation + 1].count += 1;
    // ����ǰ����������������ļ�������
    for (i = 0; i <= generation; i++)
        _PyRuntime.gc.generations[i].count = 0;

    /* ��������Ĵ����ϲ�����ǰҪ�ռ��Ĵ����� */
    for (i = 0; i < generation; i++) {
        // ���� gc_list_merge ����������������Ķ�������ϲ�����ǰ����
        gc_list_merge(GEN_HEAD(i), GEN_HEAD(generation));
    }

    // ��ȡ��ǰҪ���Ĵ����Ķ�������ͷ
    young = GEN_HEAD(generation);
    // �����ǰ����������ߴ�������ȡ��һ�����ϴ����Ķ�������ͷ
    if (generation < NUM_GENERATIONS - 1)
        old = GEN_HEAD(generation + 1);
    else
        // �������ߴ�����old ָ�� young
        old = young;

    /* ʹ�� ob_refcnt �� gc_refs������������������Щ������ԴӼ����ⲿ����
     * ���������Ǽ����ڵ���������ʱ�����ü������� 0����
     */
     // �������ü���
    update_refs(young);
    // ��ȥ�����ڵ����ü���
    subtract_refs(young);

    /* ���� young �ⲿ�ɴ�����ж������� young �У������������ж����� young �У��ƶ��� unreachable �С�
     * ע�⣺��ǰ�ǽ��ɴ�����ƶ����ɴＯ���С���ͨ������������ǿɴ�ģ�
     * �����ƶ����ɴ�������Ч��
     */
     // ��ʼ�� unreachable ����
    gc_list_init(&unreachable);
    // �����ɴ����� young �ƶ��� unreachable
    move_unreachable(young, &unreachable);

    /* ���ɴ�����ƶ�����һ�� */
    if (young != old) {
        if (generation == NUM_GENERATIONS - 2) {
            // ����ǵ����ڶ��������ӳ��������ڴ��������ļ���
            _PyRuntime.gc.long_lived_pending += gc_list_size(young);
        }
        // �� young �еĿɴ����ϲ��� old ��
        gc_list_merge(young, old);
    }
    else {
        /* ����ֻ�������ռ�ʱȡ�������ֵ䣬�Ա�������ֵ�ѻ����μ����� #14775�� */
        // ȡ�������ֵ�
        untrack_dicts(young);
        // ���ó��������ڴ�����������
        _PyRuntime.gc.long_lived_pending = 0;
        // ���³��������ڶ�������
        _PyRuntime.gc.long_lived_total = gc_list_size(young);
    }

    /* unreachable �е����ж��������������Ӿɰ��ս��������� tp_del���ɴ�Ķ����ܰ�ȫɾ���� */
    // ��ʼ�� finalizers ����
    gc_list_init(&finalizers);
    // �����оɰ��ս����Ĳ��ɴ����� unreachable �ƶ��� finalizers
    move_legacy_finalizers(&unreachable, &finalizers);
    /* finalizers �������оɰ��ս����Ĳ��ɴ����
     * ����Щ����ɴ�Ĳ��ɴ����Ҳ�ǲ����ռ��ģ�����Ҳ�������ƶ��� finalizers �б��С�
     */
     // �ƶ��� finalizers �ɴ�Ĳ����ռ����� finalizers
    move_legacy_finalizer_reachable(&finalizers);

    // ��������� DEBUG_COLLECTABLE ����ģʽ��������ռ�������Ϣ
    if (_PyRuntime.gc.debug & DEBUG_COLLECTABLE) {
        for (gc = unreachable.gc.gc_next; gc != &unreachable; gc = gc->gc.gc_next) {
            debug_cycle("collectable", FROM_GC(gc));
        }
    }

    /* ��Ҫʱ��������ò����ûص����� */
    // ���������ã����سɹ�����Ķ����������ۼӵ� m
    m += handle_weakrefs(&unreachable, old);

    /* ���� tp_finalize �Ķ������ tp_finalize */
    // �� unreachable �еĶ�������ս���
    finalize_garbage(&unreachable);

    // ������������Ƿ���Ա�����
    if (check_garbage(&unreachable)) {
        // ������ܻ��գ����临��ϲ��� old ��
        revive_garbage(&unreachable);
        gc_list_merge(&unreachable, old);
    }
    else {
        /* �� unreachable �����еĶ������ tp_clear���⽫��������ѭ�������ơ�
         * ��Ҳ���ܵ��� finalizers �е�һЩ�����ͷš�
         */
         // ���� unreachable �ж��������� m
        m += gc_list_size(&unreachable);
        // ɾ�� unreachable �е���������
        delete_garbage(&unreachable, old);
    }

    /* �ռ��ҵ��Ĳ����ռ������ͳ����Ϣ�����������Ϣ */
    for (gc = finalizers.gc.gc_next;
        gc != &finalizers;
        gc = gc->gc.gc_next) {
        // ͳ�Ʋ����ռ���������
        n++;
        if (_PyRuntime.gc.debug & DEBUG_UNCOLLECTABLE)
            // ������� DEBUG_UNCOLLECTABLE ����ģʽ����������ռ�������Ϣ
            debug_cycle("uncollectable", FROM_GC(gc));
    }
    if (_PyRuntime.gc.debug & DEBUG_STATS) {
        // ��¼�������ս���ʱ��
        _PyTime_t t2 = _PyTime_GetMonotonicClock();

        if (m == 0 && n == 0)
            PySys_WriteStderr("gc: done");
        else
            PySys_FormatStderr(
                "gc: done, %zd unreachable, %zd uncollectable",
                n + m, n);
        // ����������պ�ʱ
        PySys_WriteStderr(", %.4fs elapsed\n",
            _PyTime_AsSecondsDouble(t2 - t1));
    }

    /* �������ռ������е�ʵ�����ӵ� Python �ɴ�������б��С�
     * �������Ա��ִ����������͵Ľṹ�����Ǳ��봦��������⡣
     */
     // ����ɰ��ս���
    handle_legacy_finalizers(&finalizers, old);

    /* �����ռ���ߴ���ʱ��������б� */
    if (generation == NUM_GENERATIONS - 1) {
        // ��������б�
        clear_freelists();
    }

    // ����������չ������Ƿ����쳣
    if (PyErr_Occurred()) {
        if (nofail) {
            // ��� nofail Ϊ�棬����쳣��Ϣ
            PyErr_Clear();
        }
        else {
            if (gc_str == NULL)
                // ����һ�� Unicode �����ʾ "garbage collection"
                gc_str = PyUnicode_FromString("garbage collection");
            // ���δ�����쳣��Ϣ
            PyErr_WriteUnraisable(gc_str);
            // �׳���������
            Py_FatalError("unexpected exception during garbage collection");
        }
    }

    /* ����ͳ����Ϣ */
    if (n_collected)
        // ��� n_collected ��Ϊ�գ����ɹ����յĶ���������ֵ����
        *n_collected = m;
    if (n_uncollectable)
        // ��� n_uncollectable ��Ϊ�գ����޷����յĶ���������ֵ����
        *n_uncollectable = n;
    // ���ӵ�ǰ�������ռ�����
    stats->collections++;
    // �ۼӳɹ����յĶ�������
    stats->collected += m;
    // �ۼ��޷����յĶ�������
    stats->uncollectable += n;

    // ��� DTrace ������ GC_DONE ̽�룬������̽��
    if (PyDTrace_GC_DONE_ENABLED())
        PyDTrace_GC_DONE(n + m);

    // ���سɹ����պ��޷����յĶ�������
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
  * collect_with_callback ��������ִ���������ղ��������ڲ���ǰ����ûص��������Լ�¼�����������յ������Ϣ��
  *
  * @param generation һ����������ʾҪ�����������յ���ʼ��������������ͨ�����ָ��������ʼ���������漰������Ĵ�����
  * @return ����һ�� Py_ssize_t ���͵�ֵ����ʾ�������ղ����Ľ����ͨ���ǻ��յĶ���������
  */
static Py_ssize_t
collect_with_callback(int generation)
{
    // ���� Py_ssize_t ���͵ı��������ڴ洢��ͬ���������ս����Ϣ
    // result ���ڴ洢�������ղ��������ս����ͨ���ǻ��յĶ�������
    Py_ssize_t result;
    // collected ���ڴ洢�������������гɹ����յĶ�������
    Py_ssize_t collected;
    // uncollectable ���ڴ洢���������������޷����յĶ�����������Щ����������ڴ���ѭ�����õ�ԭ���޷�����������
    Py_ssize_t uncollectable;

    // ���� invoke_gc_callback ���������������տ�ʼǰ�����ص���
    // ��һ������ "start" �ǻص��ı�ʶ������ʾ�������ղ�����ʼ
    // �ڶ������� generation ����Ҫ�����������յ���ʼ����
    // �������͵��ĸ�������Ϊ 0����Ϊ�ڿ�ʼ�׶λ�û��ʵ�ʵĻ���������Ϣ
    invoke_gc_callback("start", generation, 0, 0);

    // ���� collect ����ִ��ʵ�ʵ��������ղ�����
    // ��һ������ generation ��ʾ���ĸ�������ʼ������������
    // �ڶ������� &collected ��һ��ָ�� collected ������ָ�룬������ collect �������ɹ����յĶ��������洢���ñ�����
    // ���������� &uncollectable ��һ��ָ�� uncollectable ������ָ�룬������ collect �������޷����յĶ��������洢���ñ�����
    // ���ĸ����� 0 ������һ����־λ�����ڴ��ݶ���Ļ���ѡ������ﴫ�� 0 ��ʾʹ��Ĭ��ѡ��
    result = collect(generation, &collected, &uncollectable, 0);

    // ���� invoke_gc_callback ���������������ս����󴥷��ص���
    // ��һ������ "stop" �ǻص��ı�ʶ������ʾ�������ղ�������
    // �ڶ������� generation ���ݱ��ν����������յ���ʼ����
    // ���������� collected ���ݱ������������гɹ����յĶ�������
    // ���ĸ����� uncollectable ���ݱ��������������޷����յĶ�������
    invoke_gc_callback("stop", generation, collected, uncollectable);

    // �����������ղ��������ս���������յĶ�������
    return result;
}

// �ú������ڴ��� Python �������ջ����еķִ����ղ�����
// ����ݸ����Ķ����������ֵ�������������Щ���Ķ�������������ա�
// ����ֵ�Ǳ����������չ����л��յĶ���������
static Py_ssize_t
collect_generations(void)
{
    // ����һ���������� i�����ڱ�����ͬ���������մ���
    int i;
    // ����һ�� Py_ssize_t ���͵ı��� n�����ڼ�¼�����������չ����л��յĶ�����������ʼ��Ϊ 0��
    Py_ssize_t n = 0;
    /*
     * ���Ҷ������������ֵ�����ϵĴ���������Ĵ�����
     * �ô��Լ���������Ĵ��еĶ��󶼽������ա�
     * �����ϵĴ���ʼ��ǰ��������Ϊ�ִ����ջ����У�
     * �����ϵĴ���Ҫ����ʱ��ͨ��Ҳ���������ս�������Ķ���
     */
    for (i = NUM_GENERATIONS - 1; i >= 0; i--) {
        // ��鵱ǰ���Ķ�������Ƿ񳬹��˸ô�����ֵ��
        if (_PyRuntime.gc.generations[i].count > _PyRuntime.gc.generations[i].threshold) {
            /*
             * �����ڸ��ٶ��������϶�ʱ���ֶ��������˻����⡣
             * �ο��ļ���ͷ��ע�ͺ����� #4074 �˽������Ϣ��
             * �����ǰ�����ϵĴ���i ���� NUM_GENERATIONS - 1����
             * ���ҳ��������ڴ�������������С�ڳ��������ڶ����������ķ�֮һ��
             * ���������λ��գ����������һ��������Ĵ���
             * ����һ�������Ż����ԣ���ֹ����Ҫ���������ղ�����
             */
            if (i == NUM_GENERATIONS - 1
                && _PyRuntime.gc.long_lived_pending < _PyRuntime.gc.long_lived_total / 4)
                continue;
            // ���� collect_with_callback �����Դӵ� i ����������Ĵ������������ղ�����
            // �������յĶ���������ֵ������ n��
            // collect_with_callback �������ܻ�ִ��һЩ�ص�������
            // �����ڻ���ǰ��ִ���ض��ĺ�����
            n = collect_with_callback(i);
            // һ���ҵ���Ҫ���յĴ���ִ���˻��ղ�����������ѭ����
            // ��Ϊ�Ѿ�����ش��Ķ�������˻��ա�
            break;
        }
    }
    // ���ر����������չ����л��յĶ���������
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
_PyObject_GC_Alloc�����ڷ��������������ͷ���� Python �����ڴ�
���� use_calloc ��һ������������ָʾ�Ƿ�ʹ�� calloc �����ڴ����
���� basicsize ��һ�� size_t ���͵�ֵ����ʾҪ����Ķ���Ļ�����С
*/
static PyObject *
_PyObject_GC_Alloc(int use_calloc, size_t basicsize)
{
    // ����һ��ָ�� PyObject ���͵�ָ�� op�����ڴ洢���շ���� Python ����
    PyObject *op;
    // ����һ��ָ�� PyGC_Head ���͵�ָ�� g�����ڹ�����������ͷ����Ϣ
    PyGC_Head *g;
    // ����һ�� size_t ���͵ı��� size�����ڴ洢Ҫ��������ڴ��С
    size_t size;
    
    // ��� basicsize �Ƿ������� basicsize ���� PyGC_Head �Ĵ�С������ PY_SSIZE_T_MAX
    // ˵�����ܻᵼ�������������ʱ���� PyErr_NoMemory ���������ڴ治����󲢷��� NULL

    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head))
        return PyErr_NoMemory();

    // ����Ҫ��������ڴ��С���� PyGC_Head �Ĵ�С���϶���Ļ�����С
    size = sizeof(PyGC_Head) + basicsize;
    // ���� use_calloc ��ֵѡ��ͬ���ڴ���䷽ʽ
    if (use_calloc)
        // ��� use_calloc Ϊ�棬ʹ�� PyObject_Calloc ���������ڴ�
        // �ú����Ὣ������ڴ��ʼ��Ϊ 0
        g = (PyGC_Head *)PyObject_Calloc(1, size);
    else
        // ��� use_calloc Ϊ�٣�ʹ�� PyObject_Malloc ���������ڴ�
        // �ú���������ڴ治�ᱻ��ʼ��
        g = (PyGC_Head *)PyObject_Malloc(size);

    // ����ڴ�����Ƿ�ɹ������ g Ϊ NULL��˵���ڴ����ʧ��
    // ���� PyErr_NoMemory ���������ڴ治����󲢷��� NULL
    
    if (g == NULL)
        return PyErr_NoMemory();
    // ����������ͷ�������ü�����ʼ��Ϊ 0
    g->gc.gc_refs = 0;
    // ���� _PyGCHead_SET_REFS �꽫��������ͷ�������ü������Ϊ GC_UNTRACKED
    // ��ʾ�ö���Ŀǰδ���������ջ��Ƹ���

    _PyGCHead_SET_REFS(g, GC_UNTRACKED);
    // ���ӵ� 0 ���������մ��ļ���
    // ��ʾ�·�����һ��������������ͷ���Ķ���

    _PyRuntime.gc.generations[0].count++; /* number of allocated GC objects */
    // ����Ƿ������������յ�����
    if (_PyRuntime.gc.generations[0].count > _PyRuntime.gc.generations[0].threshold &&
        _PyRuntime.gc.enabled &&
        _PyRuntime.gc.generations[0].threshold &&
        !_PyRuntime.gc.collecting &&
        !PyErr_Occurred()) {
        // ��������������� collecting ��־����Ϊ 1����ʾ���ڽ�����������
        _PyRuntime.gc.collecting = 1;
        // ���� collect_generations ���������������ղ���
        collect_generations();
        // ����������ɺ󣬽� collecting ��־����Ϊ 0����ʾ�������ս���
        _PyRuntime.gc.collecting = 0;
    }
    // ���� FROM_GC �꽫 PyGC_Head ָ��ת��Ϊ PyObject ָ��
    op = FROM_GC(g);
    // ���ط���� Python ����ָ��
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

// _PyObject_GC_New �������ڴ���һ��֧���������յ� Python ����
// ���� tp ��һ��ָ�� PyTypeObject ���͵�ָ�룬����Ҫ������������͡�
PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    // ���� _PyObject_GC_Malloc ����Ϊ��������ڴ档
    // _PyObject_SIZE(tp) ���ڼ�������Ͷ���������ڴ��С�����ῼ�Ƕ�������ͺͿ��ܵĶ��⿪����
    // ��������һ��ָ���·����ڴ�� PyObject ָ�롣
    PyObject *op = _PyObject_GC_Malloc(_PyObject_SIZE(tp));
    // ����ڴ�����Ƿ�ɹ������ op ��Ϊ NULL��˵���ڴ����ɹ���
    if (op != NULL)
        // ���� PyObject_INIT �������·���Ķ�����г�ʼ����
        // �ú����Ὣ�������������Ϊ tp ��ָ������ͣ�������һЩ��Ҫ�ĳ�ʼ��������
        // ��ʼ����ɺ󣬷��س�ʼ����Ķ���ָ�롣
        op = PyObject_INIT(op, tp);
    // ���ش�������ʼ���õĶ���ָ�룬����ڴ����ʧ���򷵻� NULL��
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
