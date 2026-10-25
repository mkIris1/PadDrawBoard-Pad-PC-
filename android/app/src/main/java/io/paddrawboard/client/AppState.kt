package io.paddrawboard.client

import java.util.concurrent.CopyOnWriteArrayList

enum class ConnectionState { STOPPED, CONNECTING, CONNECTED, RECONNECTING, ERROR }

data class ClientState(
    val connection: ConnectionState = ConnectionState.STOPPED,
    val status: String = "Idle",
    val palmRejectionEnabled: Boolean = true,
    val capabilitySummary: String = "Not probed",
    val droppedVideoFrames: Long = 0,
    val lastError: String? = null,
)

class AppState {
    @Volatile private var value = ClientState()
    private val listeners = CopyOnWriteArrayList<(ClientState) -> Unit>()
    fun snapshot(): ClientState = value
    @Synchronized fun update(transform: (ClientState) -> ClientState) {
        value = transform(value)
        listeners.forEach { it(value) }
    }
    fun observe(listener: (ClientState) -> Unit): AutoCloseable {
        listeners += listener; listener(value)
        return AutoCloseable { listeners -= listener }
    }
}
