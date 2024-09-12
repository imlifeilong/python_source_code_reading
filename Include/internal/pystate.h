#ifndef Py_INTERNAL_PYSTATE_H
#define Py_INTERNAL_PYSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pystate.h"
#include "pyatomic.h"
#include "pythread.h"

#include "internal/mem.h"
#include "internal/ceval.h"
#include "internal/warnings.h"


/* GIL state */

struct _gilstate_runtime_state {
    int check_enabled;
    /* Assuming the current thread holds the GIL, this is the
       PyThreadState for the current thread. */
    _Py_atomic_address tstate_current;
    PyThreadFrameGetter getframe;
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
    Py_tss_t autoTSSkey;
};

/* hook for PyEval_GetFrame(), requested for Psyco */
#define _PyThreadState_GetFrame _PyRuntime.gilstate.getframe

/* Issue #26558: Flag to disable PyGILState_Check().
   If set to non-zero, PyGILState_Check() always return 1. */
#define _PyGILState_check_enabled _PyRuntime.gilstate.check_enabled


typedef struct {
    /* Full path to the Python program */
    wchar_t *program_full_path;
    wchar_t *prefix;
#ifdef MS_WINDOWS
    wchar_t *dll_path;
#else
    wchar_t *exec_prefix;
#endif
    /* Set by Py_SetPath(), or computed by _PyPathConfig_Init() */
    wchar_t *module_search_path;
    /* Python program name */
    wchar_t *program_name;
    /* Set by Py_SetPythonHome() or PYTHONHOME environment variable */
    wchar_t *home;
} _PyPathConfig;

#define _PyPathConfig_INIT {.module_search_path = NULL}
/* Note: _PyPathConfig_INIT sets other fields to 0/NULL */

PyAPI_DATA(_PyPathConfig) _Py_path_config;

PyAPI_FUNC(_PyInitError) _PyPathConfig_Calculate(
    _PyPathConfig *config,
    const _PyCoreConfig *core_config);
PyAPI_FUNC(void) _PyPathConfig_Clear(_PyPathConfig *config);


/* interpreter state */

PyAPI_FUNC(PyInterpreterState *) _PyInterpreterState_LookUpID(PY_INT64_T);

PyAPI_FUNC(int) _PyInterpreterState_IDInitref(PyInterpreterState *);
PyAPI_FUNC(void) _PyInterpreterState_IDIncref(PyInterpreterState *);
PyAPI_FUNC(void) _PyInterpreterState_IDDecref(PyInterpreterState *);

/* Full Python runtime state */
// 这个结构体 _PyRuntimeState 是 CPython 解释器的运行时状态，
// 包含了多个与解释器执行和管理相关的子状态。
typedef struct pyruntimestate {
    int initialized; // 如果解释器已初始化，则为1，否则为0。
    int core_initialized; // 如果解释器的核心部分已初始化，则为1，否则为0。
    // 用于追踪正在被终止的线程状态
    PyThreadState *finalizing; // 指向当前正在终止的线程状态对象（如果有的话）。

    // 解释器相关的结构体
    struct pyinterpreters {
        PyThread_type_lock mutex; // 用于保护下面的解释器列表的互斥锁。
        PyInterpreterState *head; // 链表头，指向第一个解释器状态对象。
        PyInterpreterState *main; // 指向主解释器状态对象，即ID为0的解释器。
        /* _next_interp_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Init(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. 
            _next_interp_id 是一个自动编号的序列，通常是小整数。
           它在 _PyInterpreterState_Init() 中初始化（该函数在 Py_Initialize() 中调用），
           并在 PyInterpreterState_New() 中使用。负的解释器ID表示发生了错误。
           主解释器的ID永远为0。ID溢出时会导致RuntimeError。
           如果以后这成为一个问题，我们可以调整，例如使用Python的int类型。*/
        int64_t next_id; // 下一个将要分配的解释器ID。
    } interpreters;

    // 用于存储退出时调用的函数指针数组
#define NEXITFUNCS 32 // 允许注册的最大退出函数数量。
    void (*exitfuncs[NEXITFUNCS])(void); // 保存退出时需要执行的函数指针数组。
    int nexitfuncs; // 当前注册的退出函数数量。

    // 内存管理和垃圾回收的状态信息
    struct _gc_runtime_state gc; // 垃圾回收器的运行时状态。
    struct _warnings_runtime_state warnings;  // 警告系统的运行时状态。
    struct _ceval_runtime_state ceval; // CPython的字节码执行器的运行时状态。
    struct _gilstate_runtime_state gilstate; // GIL（全局解释器锁）的运行时状态。

    // XXX Consolidate globals found via the check-c-globals script.
} _PyRuntimeState;

#define _PyRuntimeState_INIT {.initialized = 0, .core_initialized = 0}
/* Note: _PyRuntimeState_INIT sets other fields to 0/NULL */

PyAPI_DATA(_PyRuntimeState) _PyRuntime;
PyAPI_FUNC(_PyInitError) _PyRuntimeState_Init(_PyRuntimeState *);
PyAPI_FUNC(void) _PyRuntimeState_Fini(_PyRuntimeState *);

/* Initialize _PyRuntimeState.
   Return NULL on success, or return an error message on failure. */
PyAPI_FUNC(_PyInitError) _PyRuntime_Initialize(void);

PyAPI_FUNC(void) _PyRuntime_Finalize(void);


#define _Py_CURRENTLY_FINALIZING(tstate) \
    (_PyRuntime.finalizing == tstate)


/* Other */

PyAPI_FUNC(_PyInitError) _PyInterpreterState_Enable(_PyRuntimeState *);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYSTATE_H */
