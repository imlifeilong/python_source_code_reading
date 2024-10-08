
/* This code implemented by Dag.Gruneau@elsa.preseco.comm.se */
/* Fast NonRecursiveMutex support by Yakov Markovitch, markovitch@iso.ru */
/* Eliminated some memory leaks, gsw@agere.com */

#include <windows.h>
#include <limits.h>
#ifdef HAVE_PROCESS_H
#include <process.h>
#endif

/* options */
#ifndef _PY_USE_CV_LOCKS
#define _PY_USE_CV_LOCKS 1     /* use locks based on cond vars */
#endif

/* Now, define a non-recursive mutex using either condition variables
 * and critical sections (fast) or using operating system mutexes
 * (slow)
 */

#if _PY_USE_CV_LOCKS

#include "condvar.h"

typedef struct _NRMUTEX
{
    PyMUTEX_T cs;
    PyCOND_T cv;
    int locked;
} NRMUTEX;
typedef NRMUTEX *PNRMUTEX;

PNRMUTEX
AllocNonRecursiveMutex()
{
    PNRMUTEX m = (PNRMUTEX)PyMem_RawMalloc(sizeof(NRMUTEX));
    if (!m)
        return NULL;
    if (PyCOND_INIT(&m->cv))
        goto fail;
    if (PyMUTEX_INIT(&m->cs)) {
        PyCOND_FINI(&m->cv);
        goto fail;
    }
    m->locked = 0;
    return m;
fail:
    PyMem_RawFree(m);
    return NULL;
}

VOID
FreeNonRecursiveMutex(PNRMUTEX mutex)
{
    if (mutex) {
        PyCOND_FINI(&mutex->cv);
        PyMUTEX_FINI(&mutex->cs);
        PyMem_RawFree(mutex);
    }
}

DWORD
EnterNonRecursiveMutex(PNRMUTEX mutex, DWORD milliseconds)
{
    DWORD result = WAIT_OBJECT_0;
    if (PyMUTEX_LOCK(&mutex->cs))
        return WAIT_FAILED;
    if (milliseconds == INFINITE) {
        while (mutex->locked) {
            if (PyCOND_WAIT(&mutex->cv, &mutex->cs)) {
                result = WAIT_FAILED;
                break;
            }
        }
    } else if (milliseconds != 0) {
        /* wait at least until the target */
        ULONGLONG now, target = GetTickCount64() + milliseconds;
        while (mutex->locked) {
            if (PyCOND_TIMEDWAIT(&mutex->cv, &mutex->cs, (long long)milliseconds*1000) < 0) {
                result = WAIT_FAILED;
                break;
            }
            now = GetTickCount64();
            if (target <= now)
                break;
            milliseconds = (DWORD)(target-now);
        }
    }
    if (!mutex->locked) {
        mutex->locked = 1;
        result = WAIT_OBJECT_0;
    } else if (result == WAIT_OBJECT_0)
        result = WAIT_TIMEOUT;
    /* else, it is WAIT_FAILED */
    PyMUTEX_UNLOCK(&mutex->cs); /* must ignore result here */
    return result;
}

BOOL
LeaveNonRecursiveMutex(PNRMUTEX mutex)
{
    BOOL result;
    if (PyMUTEX_LOCK(&mutex->cs))
        return FALSE;
    mutex->locked = 0;
    /* condvar APIs return 0 on success. We need to return TRUE on success. */
    result = !PyCOND_SIGNAL(&mutex->cv);
    PyMUTEX_UNLOCK(&mutex->cs);
    return result;
}

#else /* if ! _PY_USE_CV_LOCKS */

/* NR-locks based on a kernel mutex */
#define PNRMUTEX HANDLE

PNRMUTEX
AllocNonRecursiveMutex()
{
    return CreateSemaphore(NULL, 1, 1, NULL);
}

VOID
FreeNonRecursiveMutex(PNRMUTEX mutex)
{
    /* No in-use check */
    CloseHandle(mutex);
}

DWORD
EnterNonRecursiveMutex(PNRMUTEX mutex, DWORD milliseconds)
{
    return WaitForSingleObjectEx(mutex, milliseconds, FALSE);
}

BOOL
LeaveNonRecursiveMutex(PNRMUTEX mutex)
{
    return ReleaseSemaphore(mutex, 1, NULL);
}
#endif /* _PY_USE_CV_LOCKS */

unsigned long PyThread_get_thread_ident(void);

/*
 * Initialization of the C package, should not be needed.
 */
static void
PyThread__init_thread(void)
{
}

/*
 * Thread support.
 */

typedef struct {
    void (*func)(void*);
    void *arg;
} callobj;

/* thunker to call adapt between the function type used by the system's
thread start function and the internally used one. */
static unsigned __stdcall
bootstrap(void *call)
{
    callobj *obj = (callobj*)call;
    void (*func)(void*) = obj->func;
    void *arg = obj->arg;
    HeapFree(GetProcessHeap(), 0, obj);
    func(arg);
    return 0;
}

// 在Windows上创建一个新的线程，并返回线程的ID。
// 如果线程创建失败，返回一个无效的线程ID。
// 在执行过程中，函数会分配内存给线程的参数对象，并根据线程的栈大小启动线程。
unsigned long
PyThread_start_new_thread(void (*func)(void *), void *arg)
{
    HANDLE hThread; //  用于存储新创建线程的句柄。
    unsigned threadID; // 用于存储新线程的ID。
    callobj *obj; // 用于存储传递给新线程的函数和参数。

    // 使用dprintf函数打印调试信息，表明PyThread_start_new_thread函数已被调用。输出包括当前线程的标识符。
    dprintf(("%lu: PyThread_start_new_thread called\n",
             PyThread_get_thread_ident()));
    // 检查全局变量initialized是否为false，如果是，则调用PyThread_init_thread()函数对线程系统进行初始化。
    // 这是为了确保线程系统在使用前已经初始化。
    if (!initialized)
        PyThread_init_thread();

    // 使用HeapAlloc函数从进程的堆中分配内存，创建一个callobj结构体。
    // 这个结构体用于存储线程函数func和它的参数arg。
    // 如果分配失败，则返回一个无效的线程ID（PYTHREAD_INVALID_THREAD_ID）。
    obj = (callobj*)HeapAlloc(GetProcessHeap(), 0, sizeof(*obj));
    if (!obj)
        return PYTHREAD_INVALID_THREAD_ID;
    // 将传入的函数指针func和参数arg存储到obj结构体中，以便新线程启动时可以调用。
    obj->func = func;
    obj->arg = arg;
    // 通过调用PyThreadState_GET()获取当前线程的状态。如果获取到有效的线程状态，则从中提取出解释器设置的线程栈大小。
    PyThreadState *tstate = PyThreadState_GET();
    // 
    size_t stacksize = tstate ? tstate->interp->pythread_stacksize : 0;
    // 调用_beginthreadex函数创建一个新线程
    // 栈大小（stacksize），如果没有指定，则使用默认大小
    // 线程入口函数（bootstrap），即线程的主函数。
    // 传递给新线程的参数（obj，其中包含了线程的函数和参数）。
    // 获取新线程的ID并存储在threadID变量中。
    hThread = (HANDLE)_beginthreadex(0,
                      Py_SAFE_DOWNCAST(stacksize, Py_ssize_t, unsigned int),
                      bootstrap, obj,
                      0, &threadID);
    // 如果_beginthreadex返回0，表示线程创建失败。此时，会打印错误信息（包括错误码errno），
    // 释放之前分配的内存obj，并将threadID设置为无效值（(unsigned)-1）。
    if (hThread == 0) {
        /* I've seen errno == EAGAIN here, which means "there are
         * too many threads".
         */
        int e = errno;
        dprintf(("%lu: PyThread_start_new_thread failed, errno %d\n",
                 PyThread_get_thread_ident(), e));
        threadID = (unsigned)-1;
        HeapFree(GetProcessHeap(), 0, obj);
    }
    else {
        // 如果线程创建成功，会打印成功的调试信息（包括新线程的句柄），
        // 并使用CloseHandle函数关闭线程句柄，因为在线程创建后不再需要它。
        dprintf(("%lu: PyThread_start_new_thread succeeded: %p\n",
                 PyThread_get_thread_ident(), (void*)hThread));
        CloseHandle(hThread);
    }
    return threadID;
}

/*
 * Return the thread Id instead of a handle. The Id is said to uniquely identify the
 * thread in the system
 */
unsigned long
PyThread_get_thread_ident(void)
{
    if (!initialized)
        PyThread_init_thread();

    return GetCurrentThreadId();
}

void
PyThread_exit_thread(void)
{
    dprintf(("%lu: PyThread_exit_thread called\n", PyThread_get_thread_ident()));
    if (!initialized)
        exit(0);
    _endthreadex(0);
}

/*
 * Lock support. It has to be implemented as semaphores.
 * I [Dag] tried to implement it with mutex but I could find a way to
 * tell whether a thread already own the lock or not.
 */
PyThread_type_lock
PyThread_allocate_lock(void)
{
    PNRMUTEX aLock;

    dprintf(("PyThread_allocate_lock called\n"));
    if (!initialized)
        PyThread_init_thread();

    aLock = AllocNonRecursiveMutex() ;

    dprintf(("%lu: PyThread_allocate_lock() -> %p\n", PyThread_get_thread_ident(), aLock));

    return (PyThread_type_lock) aLock;
}

void
PyThread_free_lock(PyThread_type_lock aLock)
{
    dprintf(("%lu: PyThread_free_lock(%p) called\n", PyThread_get_thread_ident(),aLock));

    FreeNonRecursiveMutex(aLock) ;
}

/*
 * Return 1 on success if the lock was acquired
 *
 * and 0 if the lock was not acquired. This means a 0 is returned
 * if the lock has already been acquired by this thread!
 */
PyLockStatus
PyThread_acquire_lock_timed(PyThread_type_lock aLock,
                            PY_TIMEOUT_T microseconds, int intr_flag)
{
    /* Fow now, intr_flag does nothing on Windows, and lock acquires are
     * uninterruptible.  */
    PyLockStatus success;
    PY_TIMEOUT_T milliseconds;

    if (microseconds >= 0) {
        milliseconds = microseconds / 1000;
        if (microseconds % 1000 > 0)
            ++milliseconds;
        if (milliseconds > PY_DWORD_MAX) {
            Py_FatalError("Timeout larger than PY_TIMEOUT_MAX");
        }
    }
    else {
        milliseconds = INFINITE;
    }

    dprintf(("%lu: PyThread_acquire_lock_timed(%p, %lld) called\n",
             PyThread_get_thread_ident(), aLock, microseconds));

    if (aLock && EnterNonRecursiveMutex((PNRMUTEX)aLock,
                                        (DWORD)milliseconds) == WAIT_OBJECT_0) {
        success = PY_LOCK_ACQUIRED;
    }
    else {
        success = PY_LOCK_FAILURE;
    }

    dprintf(("%lu: PyThread_acquire_lock(%p, %lld) -> %d\n",
             PyThread_get_thread_ident(), aLock, microseconds, success));

    return success;
}
int
PyThread_acquire_lock(PyThread_type_lock aLock, int waitflag)
{
    return PyThread_acquire_lock_timed(aLock, waitflag ? -1 : 0, 0);
}

void
PyThread_release_lock(PyThread_type_lock aLock)
{
    dprintf(("%lu: PyThread_release_lock(%p) called\n", PyThread_get_thread_ident(),aLock));

    if (!(aLock && LeaveNonRecursiveMutex((PNRMUTEX) aLock)))
        dprintf(("%lu: Could not PyThread_release_lock(%p) error: %ld\n", PyThread_get_thread_ident(), aLock, GetLastError()));
}

/* minimum/maximum thread stack sizes supported */
#define THREAD_MIN_STACKSIZE    0x8000          /* 32 KiB */
#define THREAD_MAX_STACKSIZE    0x10000000      /* 256 MiB */

/* set the thread stack size.
 * Return 0 if size is valid, -1 otherwise.
 */
static int
_pythread_nt_set_stacksize(size_t size)
{
    /* set to default */
    if (size == 0) {
        PyThreadState_GET()->interp->pythread_stacksize = 0;
        return 0;
    }

    /* valid range? */
    if (size >= THREAD_MIN_STACKSIZE && size < THREAD_MAX_STACKSIZE) {
        PyThreadState_GET()->interp->pythread_stacksize = size;
        return 0;
    }

    return -1;
}

#define THREAD_SET_STACKSIZE(x) _pythread_nt_set_stacksize(x)


/* Thread Local Storage (TLS) API

   This API is DEPRECATED since Python 3.7.  See PEP 539 for details.
*/

int
PyThread_create_key(void)
{
    DWORD result = TlsAlloc();
    if (result == TLS_OUT_OF_INDEXES)
        return -1;
    return (int)result;
}

void
PyThread_delete_key(int key)
{
    TlsFree(key);
}

int
PyThread_set_key_value(int key, void *value)
{
    BOOL ok = TlsSetValue(key, value);
    return ok ? 0 : -1;
}

void *
PyThread_get_key_value(int key)
{
    /* because TLS is used in the Py_END_ALLOW_THREAD macro,
     * it is necessary to preserve the windows error state, because
     * it is assumed to be preserved across the call to the macro.
     * Ideally, the macro should be fixed, but it is simpler to
     * do it here.
     */
    DWORD error = GetLastError();
    void *result = TlsGetValue(key);
    SetLastError(error);
    return result;
}

void
PyThread_delete_key_value(int key)
{
    /* NULL is used as "key missing", and it is also the default
     * given by TlsGetValue() if nothing has been set yet.
     */
    TlsSetValue(key, NULL);
}


/* reinitialization of TLS is not necessary after fork when using
 * the native TLS functions.  And forking isn't supported on Windows either.
 */
void
PyThread_ReInitTLS(void)
{
}


/* Thread Specific Storage (TSS) API

   Platform-specific components of TSS API implementation.
*/

int
PyThread_tss_create(Py_tss_t *key)
{
    assert(key != NULL);
    /* If the key has been created, function is silently skipped. */
    if (key->_is_initialized) {
        return 0;
    }

    DWORD result = TlsAlloc();
    if (result == TLS_OUT_OF_INDEXES) {
        return -1;
    }
    /* In Windows, platform-specific key type is DWORD. */
    key->_key = result;
    key->_is_initialized = 1;
    return 0;
}

void
PyThread_tss_delete(Py_tss_t *key)
{
    assert(key != NULL);
    /* If the key has not been created, function is silently skipped. */
    if (!key->_is_initialized) {
        return;
    }

    TlsFree(key->_key);
    key->_key = TLS_OUT_OF_INDEXES;
    key->_is_initialized = 0;
}

int
PyThread_tss_set(Py_tss_t *key, void *value)
{
    assert(key != NULL);
    BOOL ok = TlsSetValue(key->_key, value);
    return ok ? 0 : -1;
}

void *
PyThread_tss_get(Py_tss_t *key)
{
    assert(key != NULL);
    /* because TSS is used in the Py_END_ALLOW_THREAD macro,
     * it is necessary to preserve the windows error state, because
     * it is assumed to be preserved across the call to the macro.
     * Ideally, the macro should be fixed, but it is simpler to
     * do it here.
     */
    DWORD error = GetLastError();
    void *result = TlsGetValue(key->_key);
    SetLastError(error);
    return result;
}
