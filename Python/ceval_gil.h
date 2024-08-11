/*
 * Implementation of the Global Interpreter Lock (GIL).
 */

#include <stdlib.h>
#include <errno.h>


/* First some general settings */

#define INTERVAL (_PyRuntime.ceval.gil.interval >= 1 ? _PyRuntime.ceval.gil.interval : 1)


/*
   Notes about the implementation:

   - The GIL is just a boolean variable (locked) whose access is protected
     by a mutex (gil_mutex), and whose changes are signalled by a condition
     variable (gil_cond). gil_mutex is taken for short periods of time,
     and therefore mostly uncontended.

   - In the GIL-holding thread, the main loop (PyEval_EvalFrameEx) must be
     able to release the GIL on demand by another thread. A volatile boolean
     variable (gil_drop_request) is used for that purpose, which is checked
     at every turn of the eval loop. That variable is set after a wait of
     `interval` microseconds on `gil_cond` has timed out.

      [Actually, another volatile boolean variable (eval_breaker) is used
       which ORs several conditions into one. Volatile booleans are
       sufficient as inter-thread signalling means since Python is run
       on cache-coherent architectures only.]

   - A thread wanting to take the GIL will first let pass a given amount of
     time (`interval` microseconds) before setting gil_drop_request. This
     encourages a defined switching period, but doesn't enforce it since
     opcodes can take an arbitrary time to execute.

     The `interval` value is available for the user to read and modify
     using the Python API `sys.{get,set}switchinterval()`.

   - When a thread releases the GIL and gil_drop_request is set, that thread
     ensures that another GIL-awaiting thread gets scheduled.
     It does so by waiting on a condition variable (switch_cond) until
     the value of last_holder is changed to something else than its
     own thread state pointer, indicating that another thread was able to
     take the GIL.

     This is meant to prohibit the latency-adverse behaviour on multi-core
     machines where one thread would speculatively release the GIL, but still
     run and end up being the first to re-acquire it, making the "timeslices"
     much longer than expected.
     (Note: this mechanism is enabled with FORCE_SWITCHING above)
*/

#include "condvar.h"

#define MUTEX_INIT(mut) \
    if (PyMUTEX_INIT(&(mut))) { \
        Py_FatalError("PyMUTEX_INIT(" #mut ") failed"); };
#define MUTEX_FINI(mut) \
    if (PyMUTEX_FINI(&(mut))) { \
        Py_FatalError("PyMUTEX_FINI(" #mut ") failed"); };
#define MUTEX_LOCK(mut) \
    if (PyMUTEX_LOCK(&(mut))) { \
        Py_FatalError("PyMUTEX_LOCK(" #mut ") failed"); };
#define MUTEX_UNLOCK(mut) \
    if (PyMUTEX_UNLOCK(&(mut))) { \
        Py_FatalError("PyMUTEX_UNLOCK(" #mut ") failed"); };

#define COND_INIT(cond) \
    if (PyCOND_INIT(&(cond))) { \
        Py_FatalError("PyCOND_INIT(" #cond ") failed"); };
#define COND_FINI(cond) \
    if (PyCOND_FINI(&(cond))) { \
        Py_FatalError("PyCOND_FINI(" #cond ") failed"); };
#define COND_SIGNAL(cond) \
    if (PyCOND_SIGNAL(&(cond))) { \
        Py_FatalError("PyCOND_SIGNAL(" #cond ") failed"); };
#define COND_WAIT(cond, mut) \
    if (PyCOND_WAIT(&(cond), &(mut))) { \
        Py_FatalError("PyCOND_WAIT(" #cond ") failed"); };
#define COND_TIMED_WAIT(cond, mut, microseconds, timeout_result) \
    { \
        int r = PyCOND_TIMEDWAIT(&(cond), &(mut), (microseconds)); \
        if (r < 0) \
            Py_FatalError("PyCOND_WAIT(" #cond ") failed"); \
        if (r) /* 1 == timeout, 2 == impl. can't say, so assume timeout */ \
            timeout_result = 1; \
        else \
            timeout_result = 0; \
    } \

/*
宏定义 DEFAULT_INTERVAL，
它表示 GIL 切换的默认时间间隔，单位是微秒。
在 CPython 中，默认情况下，每隔 5 毫秒（5000 微秒），
当前持有 GIL 的线程会释放 GIL，使其他线程有机会运行。
*/ 
#define DEFAULT_INTERVAL 5000

// 静态函数声明，用于初始化 GIL 的运行时状态。
// 该函数接受一个参数 state，它是一个指向 _gil_runtime_state 结构体的指针，
// 这个结构体保存了 GIL 的状态信息。
static void _gil_initialize(struct _gil_runtime_state *state)
{
    // 这行代码定义了一个局部变量 uninitialized，它是 _Py_atomic_int 类型，并初始化为 -1。
    // _Py_atomic_int 是一个原子整数类型，用于表示在多线程环境下可以安全操作的整数。
    // -1 通常表示一个未初始化的状态。
    _Py_atomic_int uninitialized = {-1};
    // 这行代码将 state 结构体中的 locked 字段设置为 uninitialized，即 -1。
    // locked 字段表示 GIL 当前是否被锁定（即是否有线程持有 GIL）。
    // 初始化为 -1 表示 GIL 还未被任何线程锁定或尚未初始化。
    state->locked = uninitialized;
    // 这行代码将 state 结构体中的 interval 字段设置为 DEFAULT_INTERVAL，即 5000 微秒。
    // interval 字段控制线程在持有 GIL 时可以连续执行的时间段，
    // 在这个时间段结束后，GIL 会被释放，让其他线程有机会运行。
    state->interval = DEFAULT_INTERVAL;
}

// 静态函数，返回一个整数值，用于检查全局解释器锁（GIL）是否已经创建,
// 若 GIL 已创建，返回值为 1；否则返回 0。
// (static 关键字表示该函数仅在定义它的源文件内可见，不会对外部文件暴露。)
static int gil_created(void)
{
    /*
    原子加载操作：_Py_atomic_load_explicit 是一个原子操作函数，它从 _PyRuntime.ceval.gil.locked 中加载值，
        并保证该加载操作是线程安全的。这个操作可以避免竞争条件，使得不同线程可以正确读取和判断 GIL 的状态。

    加载 GIL 锁状态：&_PyRuntime.ceval.gil.locked 是指向 GIL 锁状态的指针。locked 字段表示 GIL 当前是否被持有。

    内存顺序：_Py_memory_order_acquire 是一个内存顺序参数，确保从该位置加载的值在当前线程中不会被重排，
        即所有的读取操作都会在当前的写入操作之前完成。这样保证了加载的值是最新的。

    返回判断结果：比较 locked 值是否大于或等于 0。
        如果 _Py_atomic_load_explicit 返回的值大于等于 0，说明 GIL 已经创建并初始化，此时返回 1，表示 GIL 已创建。
        否则返回 0，表示 GIL 尚未创建。
    */
    return (_Py_atomic_load_explicit(&_PyRuntime.ceval.gil.locked,
                                     _Py_memory_order_acquire)
            ) >= 0;
}

// 用于初始化全局解释器锁（GIL）
static void create_gil(void)
{
    // 初始化互斥锁：MUTEX_INIT 是用于初始化互斥锁的宏。
    // 这里初始化了 _PyRuntime.ceval.gil.mutex，它是用于保护 GIL 的关键互斥锁，
    // 确保多线程环境下对 GIL 的访问是线程安全的。
    MUTEX_INIT(_PyRuntime.ceval.gil.mutex);
    // 条件编译：FORCE_SWITCHING 是一个编译时条件宏。
    // 如果定义了 FORCE_SWITCHING，则执行以下代码块。
    // 这个宏用于在一些特定条件下强制 GIL 切换，通常在调试或特殊的运行时环境下启用。
#ifdef FORCE_SWITCHING
    // 初始化强制切换互斥锁：如果启用了 FORCE_SWITCHING，
    // 则初始化一个额外的互斥锁 _PyRuntime.ceval.gil.switch_mutex，用于在强制线程切换时保护 GIL 状态。
    MUTEX_INIT(_PyRuntime.ceval.gil.switch_mutex);
#endif
    // 初始化条件变量：COND_INIT 是用于初始化条件变量的宏。
    // _PyRuntime.ceval.gil.cond 是用于管理线程等待 GIL 的条件变量。
    // 当线程尝试获取 GIL 时，如果 GIL 被其他线程持有，线程会在此条件变量上等待。
    COND_INIT(_PyRuntime.ceval.gil.cond);
    // 条件编译：再一次检查 FORCE_SWITCHING 是否定义。
#ifdef FORCE_SWITCHING
    // 初始化强制切换条件变量：如果 FORCE_SWITCHING 被定义，
    // 则初始化一个额外的条件变量 _PyRuntime.ceval.gil.switch_cond，用于在强制 GIL 切换时同步线程。
    COND_INIT(_PyRuntime.ceval.gil.switch_cond);
#endif
    // 初始化 GIL 持有者：通过 _Py_atomic_store_relaxed 原子操作将 last_holder 初始化为 0。
    // last_holder 记录了最后一个持有 GIL 的线程的标识符，在初始化时将其设为 0 表示没有线程持有 GIL。
    _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.last_holder, 0);
    // 创建读写锁注释：_Py_ANNOTATE_RWLOCK_CREATE 是一个注释宏，用于工具（如线程检查器）了解锁的创建位置。
    // 这行代码向调试工具表明 _PyRuntime.ceval.gil.locked 是一个锁，正在创建过程中。
    _Py_ANNOTATE_RWLOCK_CREATE(&_PyRuntime.ceval.gil.locked);
    // 初始化 GIL 锁状态：通过 _Py_atomic_store_explicit 原子操作将 locked 字段初始化为 0，表示 GIL 
    // 目前未被任何线程持有。_Py_memory_order_release 确保所有之前的写操作在当前写操作之前完成，从而保证初始化的正确性。
    _Py_atomic_store_explicit(&_PyRuntime.ceval.gil.locked, 0,
                              _Py_memory_order_release);
}

static void destroy_gil(void)
{
    /* some pthread-like implementations tie the mutex to the cond
     * and must have the cond destroyed first.
     */
    COND_FINI(_PyRuntime.ceval.gil.cond);
    MUTEX_FINI(_PyRuntime.ceval.gil.mutex);
#ifdef FORCE_SWITCHING
    COND_FINI(_PyRuntime.ceval.gil.switch_cond);
    MUTEX_FINI(_PyRuntime.ceval.gil.switch_mutex);
#endif
    _Py_atomic_store_explicit(&_PyRuntime.ceval.gil.locked, -1,
                              _Py_memory_order_release);
    _Py_ANNOTATE_RWLOCK_DESTROY(&_PyRuntime.ceval.gil.locked);
}

static void recreate_gil(void)
{
    _Py_ANNOTATE_RWLOCK_DESTROY(&_PyRuntime.ceval.gil.locked);
    /* XXX should we destroy the old OS resources here? */
    create_gil();
}

static void drop_gil(PyThreadState *tstate)
{
    if (!_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.locked))
        Py_FatalError("drop_gil: GIL is not locked");
    /* tstate is allowed to be NULL (early interpreter init) */
    if (tstate != NULL) {
        /* Sub-interpreter support: threads might have been switched
           under our feet using PyThreadState_Swap(). Fix the GIL last
           holder variable so that our heuristics work. */
        _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.last_holder,
                                 (uintptr_t)tstate);
    }

    MUTEX_LOCK(_PyRuntime.ceval.gil.mutex);
    _Py_ANNOTATE_RWLOCK_RELEASED(&_PyRuntime.ceval.gil.locked, /*is_write=*/1);
    _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.locked, 0);
    COND_SIGNAL(_PyRuntime.ceval.gil.cond);
    MUTEX_UNLOCK(_PyRuntime.ceval.gil.mutex);

#ifdef FORCE_SWITCHING
    if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request) &&
        tstate != NULL)
    {
        MUTEX_LOCK(_PyRuntime.ceval.gil.switch_mutex);
        /* Not switched yet => wait */
        if (((PyThreadState*)_Py_atomic_load_relaxed(
                    &_PyRuntime.ceval.gil.last_holder)
            ) == tstate)
        {
        RESET_GIL_DROP_REQUEST();
            /* NOTE: if COND_WAIT does not atomically start waiting when
               releasing the mutex, another thread can run through, take
               the GIL and drop it again, and reset the condition
               before we even had a chance to wait for it. */
            COND_WAIT(_PyRuntime.ceval.gil.switch_cond,
                      _PyRuntime.ceval.gil.switch_mutex);
    }
        MUTEX_UNLOCK(_PyRuntime.ceval.gil.switch_mutex);
    }
#endif
}

static void take_gil(PyThreadState *tstate)
{
    int err;
    if (tstate == NULL)
        Py_FatalError("take_gil: NULL tstate");

    err = errno;
    MUTEX_LOCK(_PyRuntime.ceval.gil.mutex);

    if (!_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.locked))
        goto _ready;

    while (_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.locked)) {
        int timed_out = 0;
        unsigned long saved_switchnum;

        saved_switchnum = _PyRuntime.ceval.gil.switch_number;
        COND_TIMED_WAIT(_PyRuntime.ceval.gil.cond, _PyRuntime.ceval.gil.mutex,
                        INTERVAL, timed_out);
        /* If we timed out and no switch occurred in the meantime, it is time
           to ask the GIL-holding thread to drop it. */
        if (timed_out &&
            _Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.locked) &&
            _PyRuntime.ceval.gil.switch_number == saved_switchnum) {
            SET_GIL_DROP_REQUEST();
        }
    }
_ready:
#ifdef FORCE_SWITCHING
    /* This mutex must be taken before modifying
       _PyRuntime.ceval.gil.last_holder (see drop_gil()). */
    MUTEX_LOCK(_PyRuntime.ceval.gil.switch_mutex);
#endif
    /* We now hold the GIL */
    _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.locked, 1);
    _Py_ANNOTATE_RWLOCK_ACQUIRED(&_PyRuntime.ceval.gil.locked, /*is_write=*/1);

    if (tstate != (PyThreadState*)_Py_atomic_load_relaxed(
                    &_PyRuntime.ceval.gil.last_holder))
    {
        _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.last_holder,
                                 (uintptr_t)tstate);
        ++_PyRuntime.ceval.gil.switch_number;
    }

#ifdef FORCE_SWITCHING
    COND_SIGNAL(_PyRuntime.ceval.gil.switch_cond);
    MUTEX_UNLOCK(_PyRuntime.ceval.gil.switch_mutex);
#endif
    if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request)) {
        RESET_GIL_DROP_REQUEST();
    }
    if (tstate->async_exc != NULL) {
        _PyEval_SignalAsyncExc();
    }

    MUTEX_UNLOCK(_PyRuntime.ceval.gil.mutex);
    errno = err;
}

void _PyEval_SetSwitchInterval(unsigned long microseconds)
{
    _PyRuntime.ceval.gil.interval = microseconds;
}

unsigned long _PyEval_GetSwitchInterval()
{
    return _PyRuntime.ceval.gil.interval;
}
