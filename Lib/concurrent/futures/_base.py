# Copyright 2009 Brian Quinlan. All Rights Reserved.
# Licensed to PSF under a Contributor Agreement.

__author__ = 'Brian Quinlan (brian@sweetapp.com)'

import collections
import logging
import threading
import time

FIRST_COMPLETED = 'FIRST_COMPLETED'
FIRST_EXCEPTION = 'FIRST_EXCEPTION'
ALL_COMPLETED = 'ALL_COMPLETED'
_AS_COMPLETED = '_AS_COMPLETED'

# Possible future states (for internal use by the futures package).
PENDING = 'PENDING'
RUNNING = 'RUNNING'
# The future was cancelled by the user...
CANCELLED = 'CANCELLED'
# ...and _Waiter.add_cancelled() was called by a worker.
CANCELLED_AND_NOTIFIED = 'CANCELLED_AND_NOTIFIED'
FINISHED = 'FINISHED'

_FUTURE_STATES = [
    PENDING,
    RUNNING,
    CANCELLED,
    CANCELLED_AND_NOTIFIED,
    FINISHED
]

_STATE_TO_DESCRIPTION_MAP = {
    PENDING: "pending",
    RUNNING: "running",
    CANCELLED: "cancelled",
    CANCELLED_AND_NOTIFIED: "cancelled",
    FINISHED: "finished"
}

# Logger for internal use by the futures package.
LOGGER = logging.getLogger("concurrent.futures")


class Error(Exception):
    """Base class for all future-related exceptions."""
    pass


class CancelledError(Error):
    """The Future was cancelled."""
    pass


class TimeoutError(Error):
    """The operation exceeded the given deadline."""
    pass


class _Waiter(object):
    """Provides the event that wait() and as_completed() block on."""

    def __init__(self):
        self.event = threading.Event()
        self.finished_futures = []

    def add_result(self, future):
        self.finished_futures.append(future)

    def add_exception(self, future):
        self.finished_futures.append(future)

    def add_cancelled(self, future):
        self.finished_futures.append(future)


class _AsCompletedWaiter(_Waiter):
    """Used by as_completed()."""

    def __init__(self):
        super(_AsCompletedWaiter, self).__init__()
        self.lock = threading.Lock()

    def add_result(self, future):
        with self.lock:
            super(_AsCompletedWaiter, self).add_result(future)
            self.event.set()

    def add_exception(self, future):
        with self.lock:
            super(_AsCompletedWaiter, self).add_exception(future)
            self.event.set()

    def add_cancelled(self, future):
        with self.lock:
            super(_AsCompletedWaiter, self).add_cancelled(future)
            self.event.set()


class _FirstCompletedWaiter(_Waiter):
    """Used by wait(return_when=FIRST_COMPLETED)."""

    def add_result(self, future):
        super().add_result(future)
        self.event.set()

    def add_exception(self, future):
        super().add_exception(future)
        self.event.set()

    def add_cancelled(self, future):
        super().add_cancelled(future)
        self.event.set()


class _AllCompletedWaiter(_Waiter):
    """Used by wait(return_when=FIRST_EXCEPTION and ALL_COMPLETED)."""

    def __init__(self, num_pending_calls, stop_on_exception):
        self.num_pending_calls = num_pending_calls
        self.stop_on_exception = stop_on_exception
        self.lock = threading.Lock()
        super().__init__()

    def _decrement_pending_calls(self):
        with self.lock:
            self.num_pending_calls -= 1
            if not self.num_pending_calls:
                self.event.set()

    def add_result(self, future):
        super().add_result(future)
        self._decrement_pending_calls()

    def add_exception(self, future):
        super().add_exception(future)
        if self.stop_on_exception:
            self.event.set()
        else:
            self._decrement_pending_calls()

    def add_cancelled(self, future):
        super().add_cancelled(future)
        self._decrement_pending_calls()


class _AcquireFutures(object):
    """A context manager that does an ordered acquire of Future conditions."""

    def __init__(self, futures):
        self.futures = sorted(futures, key=id)

    def __enter__(self):
        for future in self.futures:
            future._condition.acquire()

    def __exit__(self, *args):
        for future in self.futures:
            future._condition.release()


def _create_and_install_waiters(fs, return_when):
    if return_when == _AS_COMPLETED:
        waiter = _AsCompletedWaiter()
    elif return_when == FIRST_COMPLETED:
        waiter = _FirstCompletedWaiter()
    else:
        pending_count = sum(
            f._state not in [CANCELLED_AND_NOTIFIED, FINISHED] for f in fs)

        if return_when == FIRST_EXCEPTION:
            waiter = _AllCompletedWaiter(pending_count, stop_on_exception=True)
        elif return_when == ALL_COMPLETED:
            waiter = _AllCompletedWaiter(pending_count, stop_on_exception=False)
        else:
            raise ValueError("Invalid return condition: %r" % return_when)

    for f in fs:
        f._waiters.append(waiter)

    return waiter


def _yield_finished_futures(fs, waiter, ref_collect):
    """
    Iterate on the list *fs*, yielding finished futures one by one in
    reverse order.
    Before yielding a future, *waiter* is removed from its waiters
    and the future is removed from each set in the collection of sets
    *ref_collect*.

    The aim of this function is to avoid keeping stale references after
    the future is yielded and before the iterator resumes.
    """
    while fs:
        f = fs[-1]
        for futures_set in ref_collect:
            futures_set.remove(f)
        with f._condition:
            f._waiters.remove(waiter)
        del f
        # Careful not to keep a reference to the popped value
        yield fs.pop()


def as_completed(fs, timeout=None):
    """An iterator over the given futures that yields each as it completes.

    Args:
        fs: The sequence of Futures (possibly created by different Executors) to
            iterate over.
        timeout: The maximum number of seconds to wait. If None, then there
            is no limit on the wait time.

    Returns:
        An iterator that yields the given Futures as they complete (finished or
        cancelled). If any given Futures are duplicated, they will be returned
        once.

    Raises:
        TimeoutError: If the entire result iterator could not be generated
            before the given timeout.
    """
    if timeout is not None:
        end_time = timeout + time.monotonic()

    fs = set(fs)
    total_futures = len(fs)
    with _AcquireFutures(fs):
        finished = set(
            f for f in fs
            if f._state in [CANCELLED_AND_NOTIFIED, FINISHED])
        pending = fs - finished
        waiter = _create_and_install_waiters(fs, _AS_COMPLETED)
    finished = list(finished)
    try:
        yield from _yield_finished_futures(finished, waiter,
                                           ref_collect=(fs,))

        while pending:
            if timeout is None:
                wait_timeout = None
            else:
                wait_timeout = end_time - time.monotonic()
                if wait_timeout < 0:
                    raise TimeoutError(
                        '%d (of %d) futures unfinished' % (
                            len(pending), total_futures))

            waiter.event.wait(wait_timeout)

            with waiter.lock:
                finished = waiter.finished_futures
                waiter.finished_futures = []
                waiter.event.clear()

            # reverse to keep finishing order
            finished.reverse()
            yield from _yield_finished_futures(finished, waiter,
                                               ref_collect=(fs, pending))

    finally:
        # Remove waiter from unfinished futures
        for f in fs:
            with f._condition:
                f._waiters.remove(waiter)


DoneAndNotDoneFutures = collections.namedtuple(
    'DoneAndNotDoneFutures', 'done not_done')


def wait(fs, timeout=None, return_when=ALL_COMPLETED):
    """Wait for the futures in the given sequence to complete.

    Args:
        fs: The sequence of Futures (possibly created by different Executors) to
            wait upon.
        timeout: The maximum number of seconds to wait. If None, then there
            is no limit on the wait time.
        return_when: Indicates when this function should return. The options
            are:

            FIRST_COMPLETED - Return when any future finishes or is
                              cancelled.
            FIRST_EXCEPTION - Return when any future finishes by raising an
                              exception. If no future raises an exception
                              then it is equivalent to ALL_COMPLETED.
            ALL_COMPLETED -   Return when all futures finish or are cancelled.

    Returns:
        A named 2-tuple of sets. The first set, named 'done', contains the
        futures that completed (is finished or cancelled) before the wait
        completed. The second set, named 'not_done', contains uncompleted
        futures.
    """
    with _AcquireFutures(fs):
        done = set(f for f in fs
                   if f._state in [CANCELLED_AND_NOTIFIED, FINISHED])
        not_done = set(fs) - done

        if (return_when == FIRST_COMPLETED) and done:
            return DoneAndNotDoneFutures(done, not_done)
        elif (return_when == FIRST_EXCEPTION) and done:
            if any(f for f in done
                   if not f.cancelled() and f.exception() is not None):
                return DoneAndNotDoneFutures(done, not_done)

        if len(done) == len(fs):
            return DoneAndNotDoneFutures(done, not_done)

        waiter = _create_and_install_waiters(fs, return_when)

    waiter.event.wait(timeout)
    for f in fs:
        with f._condition:
            f._waiters.remove(waiter)

    done.update(waiter.finished_futures)
    return DoneAndNotDoneFutures(done, set(fs) - done)


class Future(object):
    """Represents the result of an asynchronous computation."""

    def __init__(self):
        """Initializes the future. Should not be called by clients."""
        self._condition = threading.Condition()  # 用于线程同步的条件变量
        self._state = PENDING  # 初始状态为 PENDING，表示尚未开始执行
        self._result = None  # 存储异步计算的结果
        self._exception = None  # 如果异步计算中发生异常，将异常存储在此处
        self._waiters = []  # 其他可能等待此 Future 完成的线程或对象
        self._done_callbacks = []  # 当 Future 完成后应调用的回调函数列表

    def _invoke_callbacks(self):
        for callback in self._done_callbacks:
            try:
                callback(self)  # 调用回调函数并传入当前 Future 对象
            except Exception:
                LOGGER.exception('exception calling callback for %r', self)

    def __repr__(self):
        with self._condition:
            if self._state == FINISHED:
                if self._exception:
                    return '<%s at %#x state=%s raised %s>' % (
                        self.__class__.__name__,
                        id(self),
                        _STATE_TO_DESCRIPTION_MAP[self._state],
                        self._exception.__class__.__name__)
                else:
                    return '<%s at %#x state=%s returned %s>' % (
                        self.__class__.__name__,
                        id(self),
                        _STATE_TO_DESCRIPTION_MAP[self._state],
                        self._result.__class__.__name__)
            return '<%s at %#x state=%s>' % (
                self.__class__.__name__,
                id(self),
                _STATE_TO_DESCRIPTION_MAP[self._state])

    def cancel(self):
        """Cancel the future if possible.

        Returns True if the future was cancelled, False otherwise. A future
        cannot be cancelled if it is running or has already completed.
        尝试取消 Future 的执行。
        如果成功取消，返回 True；如果 Future 已经开始执行或已经完成，则返回 False。
        """
        with self._condition:
            if self._state in [RUNNING, FINISHED]:
                return False  # 不能取消正在执行或已完成的 Future

            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                return True  # 如果已经取消，则直接返回 True

            self._state = CANCELLED  # 将状态更新为 CANCELLED
            self._condition.notify_all()  # 通知所有等待的线程

        self._invoke_callbacks()  # 调用所有注册的回调函数
        return True

    def cancelled(self):
        """Return True if the future was cancelled.
        检查 Future 是否被取消。
        """
        with self._condition:
            return self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]

    def running(self):
        """Return True if the future is currently executing.
        检查 Future 是否正在执行。
        """
        with self._condition:
            return self._state == RUNNING

    def done(self):
        """Return True of the future was cancelled or finished executing.
        检查 Future 是否已经完成（包括取消和正常完成）。
        """
        with self._condition:
            return self._state in [CANCELLED, CANCELLED_AND_NOTIFIED, FINISHED]

    def __get_result(self):
        # 获取结果，如果有异常则抛出异常。
        if self._exception:
            raise self._exception  # 如果有异常，抛出异常
        else:
            return self._result  # 否则返回结果

    def add_done_callback(self, fn):
        """Attaches a callable that will be called when the future finishes.
        当 Future 完成时，注册一个可调用对象（回调函数）。
        Args:
            fn: A callable that will be called with this future as its only
                argument when the future completes or is cancelled. The callable
                will always be called by a thread in the same process in which
                it was added. If the future has already completed or been
                cancelled then the callable will be called immediately. These
                callables are called in the order that they were added.
            fn: 一个可调用对象，当 Future 完成或取消时将被调用。回调函数将以此 Future 作为唯一参数。
        """
        with self._condition:
            if self._state not in [CANCELLED, CANCELLED_AND_NOTIFIED, FINISHED]:
                self._done_callbacks.append(fn)  # 如果 Future 尚未完成，添加回调函数到列表
                return
        try:
            fn(self)  # 如果 Future 已经完成，直接调用回调函数
        except Exception:
            LOGGER.exception('exception calling callback for %r', self)

    def result(self, timeout=None):
        """Return the result of the call that the future represents.
        返回 Future 表示的调用结果。
        Args:
            timeout: The number of seconds to wait for the result if the future
                isn't done. If None, then there is no limit on the wait time.
            timeout: 等待结果的超时时间（秒）。如果为 None，则无限期等待。
        Returns:
            The result of the call that the future represents.

        Raises:
            CancelledError: If the future was cancelled.
            TimeoutError: If the future didn't finish executing before the given
                timeout.
            Exception: If the call raised then that exception will be raised.
            CancelledError: 如果 Future 被取消。
            TimeoutError: 如果 Future 在给定的超时时间内未完成。
            Exception: 如果调用中发生异常，该异常将被抛出。
        """
        with self._condition:
            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                raise CancelledError()  # 如果 Future 被取消，抛出 CancelledError
            elif self._state == FINISHED:
                return self.__get_result()  # 如果 Future 已经完成，返回结果

            self._condition.wait(timeout)  # 等待任务完成或超时

            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                raise CancelledError()  # 如果 Future 被取消，抛出 CancelledError
            elif self._state == FINISHED:
                return self.__get_result()  # 如果 Future 已经完成，返回结果
            else:
                raise TimeoutError()  # 超时未完成，抛出 TimeoutError

    def exception(self, timeout=None):
        """Return the exception raised by the call that the future represents.
        返回 Future 表示的调用引发的异常。
        Args:
            timeout: The number of seconds to wait for the exception if the
                future isn't done. If None, then there is no limit on the wait
                time.

        Returns:
            The exception raised by the call that the future represents or None
            if the call completed without raising.
            Future 表示的调用引发的异常，或者如果调用正常完成则返回 None。
        Raises:
            CancelledError: If the future was cancelled.
            TimeoutError: If the future didn't finish executing before the given
                timeout.
        """

        with self._condition:
            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                raise CancelledError()
            elif self._state == FINISHED:
                return self._exception

            self._condition.wait(timeout)

            if self._state in [CANCELLED, CANCELLED_AND_NOTIFIED]:
                raise CancelledError()
            elif self._state == FINISHED:
                return self._exception
            else:
                raise TimeoutError()

    # The following methods should only be used by Executors and in tests.
    def set_running_or_notify_cancel(self):
        """Mark the future as running or process any cancel notifications.

        Should only be used by Executor implementations and unit tests.

        If the future has been cancelled (cancel() was called and returned
        True) then any threads waiting on the future completing (though calls
        to as_completed() or wait()) are notified and False is returned.

        If the future was not cancelled then it is put in the running state
        (future calls to running() will return True) and True is returned.

        This method should be called by Executor implementations before
        executing the work associated with this future. If this method returns
        False then the work should not be executed.

        Returns:
            False if the Future was cancelled, True otherwise.

        Raises:
            RuntimeError: if this method was already called or if set_result()
                or set_exception() was called.
        将 Future 标记为正在运行，或处理任何取消通知。

        仅应由 Executor 实现和单元测试使用。

        如果 Future 已被取消（调用了 cancel() 且返回 True），则任何等待 Future 完成的线程
        都会被通知，并返回 False。

        如果 Future 未被取消，则将其状态设置为 RUNNING（未来调用 running() 将返回 True），
        并返回 True。

        此方法应由 Executor 实现调用，在执行与此 Future 关联的工作之前调用。
        如果此方法返回 False，则不应执行该工作。

        返回:
            如果 Future 被取消，则返回 False，否则返回 True。

        异常:
            RuntimeError: 如果此方法已经被调用，或 set_result() 或 set_exception() 已被调用。

        """
        with self._condition:
            if self._state == CANCELLED:
                self._state = CANCELLED_AND_NOTIFIED # 将状态更新为已取消并通知
                for waiter in self._waiters:
                    waiter.add_cancelled(self)
                # self._condition.notify_all() is not necessary because
                # self.cancel() triggers a notification.
                # 无需调用 self._condition.notify_all()，因为 cancel() 会触发通知。
                return False
            elif self._state == PENDING:
                self._state = RUNNING # 将状态更新为正在运行
                return True
            else:
                LOGGER.critical('Future %s in unexpected state: %s',
                                id(self),
                                self._state)
                raise RuntimeError('Future in unexpected state')

    def set_result(self, result):
        """Sets the return value of work associated with the future.

        Should only be used by Executor implementations and unit tests.
        """
        with self._condition:
            self._result = result  # 设置结果
            self._state = FINISHED  # 将状态更新为已完成
            for waiter in self._waiters:
                waiter.add_result(self)
            self._condition.notify_all()  # 通知所有等待的线程
        self._invoke_callbacks()  # 调用所有注册的回调函数

    def set_exception(self, exception):
        """Sets the result of the future as being the given exception.

        Should only be used by Executor implementations and unit tests.
        """
        with self._condition:
            self._exception = exception
            self._state = FINISHED
            for waiter in self._waiters:
                waiter.add_exception(self)
            self._condition.notify_all()
        self._invoke_callbacks()


class Executor(object):
    """This is an abstract base class for concrete asynchronous executors."""

    def submit(*args, **kwargs):
        """Submits a callable to be executed with the given arguments.

        Schedules the callable to be executed as fn(*args, **kwargs) and returns
        a Future instance representing the execution of the callable.

        Returns:
            A Future representing the given call.
        """
        if len(args) >= 2:
            pass
        elif not args:
            raise TypeError("descriptor 'submit' of 'Executor' object "
                            "needs an argument")
        elif 'fn' not in kwargs:
            raise TypeError('submit expected at least 1 positional argument, '
                            'got %d' % (len(args) - 1))

        raise NotImplementedError()

    def map(self, fn, *iterables, timeout=None, chunksize=1):
        """Returns an iterator equivalent to map(fn, iter).

        Args:
            fn: A callable that will take as many arguments as there are
                passed iterables.
            timeout: The maximum number of seconds to wait. If None, then there
                is no limit on the wait time.
            chunksize: The size of the chunks the iterable will be broken into
                before being passed to a child process. This argument is only
                used by ProcessPoolExecutor; it is ignored by
                ThreadPoolExecutor.

        Returns:
            An iterator equivalent to: map(func, *iterables) but the calls may
            be evaluated out-of-order.

        Raises:
            TimeoutError: If the entire result iterator could not be generated
                before the given timeout.
            Exception: If fn(*args) raises for any values.
        """
        if timeout is not None:
            end_time = timeout + time.monotonic()

        fs = [self.submit(fn, *args) for args in zip(*iterables)]

        # Yield must be hidden in closure so that the futures are submitted
        # before the first iterator value is required.
        def result_iterator():
            try:
                # reverse to keep finishing order
                fs.reverse()
                while fs:
                    # Careful not to keep a reference to the popped future
                    if timeout is None:
                        yield fs.pop().result()
                    else:
                        yield fs.pop().result(end_time - time.monotonic())
            finally:
                for future in fs:
                    future.cancel()

        return result_iterator()

    def shutdown(self, wait=True):
        """Clean-up the resources associated with the Executor.

        It is safe to call this method several times. Otherwise, no other
        methods can be called after this one.

        Args:
            wait: If True then shutdown will not return until all running
                futures have finished executing and the resources used by the
                executor have been reclaimed.
        """
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.shutdown(wait=True)
        return False


class BrokenExecutor(RuntimeError):
    """
    Raised when a executor has become non-functional after a severe failure.
    """
