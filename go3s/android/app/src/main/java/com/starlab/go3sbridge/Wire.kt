package com.starlab.go3sbridge

import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.util.ArrayDeque

internal data class Outbound(val header: JSONObject, val payload: ByteArray = byteArrayOf(), val prepare: (() -> Unit)? = null) {
    val cost: Long = payload.size.toLong() + header.toString().toByteArray(Charsets.UTF_8).size + 8L + 256L
}

/** A single writer owns sequence allocation and writes packets in the accepted order. */
internal class WireWriter(
    output: OutputStream,
    private val onFailure: (Throwable) -> Unit,
    private val limit: Long = 64L * 1024 * 1024,
) {
    private val output = DataOutputStream(output)
    private val queue = ArrayDeque<Outbound>()
    private var bytes = 0L
    private var accepting = true
    private var terminal: List<Outbound>? = null
    private var sequence = 0L
    private val monitor = Object()
    private val thread = Thread({ run() }, "go3s-writer")

    fun start() = thread.start()

    fun offer(packet: Outbound): Boolean = synchronized(monitor) {
        if (!accepting) return false
        if (packet.payload.size > MAX_PAYLOAD || packet.header.toString().toByteArray(Charsets.UTF_8).size > MAX_HEADER || packet.cost > limit - bytes) {
            accepting = false
            return false
        }
        bytes += packet.cost
        queue.addLast(packet)
        monitor.notifyAll()
        true
    }

    fun stopIntake() = synchronized(monitor) { accepting = false }

    /** Called only after SDK stop; already accepted callbacks are drained before these events. */
    fun finish(reason: String, error: String?) = synchronized(monitor) {
        accepting = false
        if (terminal == null) {
            terminal = buildList {
                if (error != null) add(Outbound(JSONObject().put("type", "error").put("message", error)))
                add(Outbound(JSONObject().put("type", "end").put("reason", reason).put("sdk_internal_loss_observable", false)))
            }
        }
        monitor.notifyAll()
    }

    fun awaitFinished(milliseconds: Long): Boolean {
        if (Thread.currentThread() == thread) return false
        thread.join(milliseconds)
        return !thread.isAlive
    }

    private fun run() {
        try {
            while (true) {
                val item = synchronized(monitor) {
                    while (queue.isEmpty() && terminal == null) monitor.wait()
                    if (queue.isNotEmpty()) queue.removeFirst()
                    else null
                }
                if (item != null) {
                    try { write(item) } finally { synchronized(monitor) { bytes -= item.cost } }
                } else {
                    val end = synchronized(monitor) { terminal!! }
                    end.forEach { write(it) }
                    output.flush()
                    return
                }
            }
        } catch (error: Throwable) {
            stopIntake()
            onFailure(error)
        }
    }

    private fun write(packet: Outbound) {
        packet.prepare?.invoke()
        packet.header.put("v", 1).put("seq", sequence++)
        val json = packet.header.toString().toByteArray(Charsets.UTF_8)
        require(json.size <= MAX_HEADER && packet.payload.size <= MAX_PAYLOAD) { "消息超过协议限制" }
        output.writeInt(json.size)
        output.writeInt(packet.payload.size)
        output.write(json)
        output.write(packet.payload)
        output.flush()
    }

    companion object {
        const val MAX_HEADER = 1024 * 1024
        const val MAX_PAYLOAD = 32 * 1024 * 1024
    }
}

internal class WireReader(input: InputStream) {
    private val input = DataInputStream(input)
    fun read(): JSONObject {
        val headerSize = input.readInt()
        val payloadSize = input.readInt()
        require(headerSize in 1..WireWriter.MAX_HEADER) { "无效消息头长度" }
        require(payloadSize == 0) { "电脑请求不能带二进制负载" }
        val bytes = ByteArray(headerSize)
        input.readFully(bytes)
        val header = JSONObject(String(bytes, Charsets.UTF_8))
        require(header.optInt("v") == 1) { "不支持的协议版本" }
        return header
    }
}
