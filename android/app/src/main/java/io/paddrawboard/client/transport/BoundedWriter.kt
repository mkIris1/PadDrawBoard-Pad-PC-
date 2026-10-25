package io.paddrawboard.client.transport

import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

/** Never discard pointer transitions: rejection must tear down the session. */
internal fun boundedWriter() = ThreadPoolExecutor(
    1, 1, 0L, TimeUnit.MILLISECONDS, ArrayBlockingQueue<Runnable>(8),
    ThreadPoolExecutor.AbortPolicy(),
)
