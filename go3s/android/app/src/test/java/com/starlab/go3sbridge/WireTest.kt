package com.starlab.go3sbridge

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.InputStream
import java.io.IOException
import java.io.OutputStream
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class WireTest {
    private fun events(raw: ByteArray): List<Pair<JSONObject, ByteArray>> {
        val reader = DataInputStream(ByteArrayInputStream(raw))
        val result = mutableListOf<Pair<JSONObject, ByteArray>>()
        while (reader.available() > 0) {
            val headerSize = reader.readInt()
            val payloadSize = reader.readInt()
            val header = ByteArray(headerSize).also { reader.readFully(it) }
            val payload = ByteArray(payloadSize).also { reader.readFully(it) }
            result += JSONObject(String(header, Charsets.UTF_8)) to payload
        }
        return result
    }

    @Test fun overflowDrainsAcceptedRawAndReportsTerminalFailure() {
        val output = ByteArrayOutputStream()
        val writer = WireWriter(output, { throw AssertionError(it) }, 1200)
        assertTrue(writer.offer(Outbound(JSONObject().put("type", "hello"))))
        val raw = byteArrayOf(0, 1, 2, -1, 4)
        assertTrue(writer.offer(Outbound(JSONObject().put("type", "video"), raw)))
        assertFalse(writer.offer(Outbound(JSONObject().put("type", "video"), ByteArray(1200))))
        assertFalse(writer.offer(Outbound(JSONObject().put("type", "video"))))
        writer.finish("error", "overflow")
        writer.start()
        assertTrue(writer.awaitFinished(2000))
        val readback = events(output.toByteArray())
        assertEquals(listOf("hello", "video", "error", "end"), readback.map { it.first.getString("type") })
        assertEquals(listOf(0L, 1L, 2L, 3L), readback.map { it.first.getLong("seq") })
        assertArrayEquals(raw, readback[1].second)
        assertFalse(readback.last().first.getBoolean("sdk_internal_loss_observable"))
    }

    @Test fun concurrentEnqueueProducesContiguousWireSequence() {
        val output = ByteArrayOutputStream()
        val writer = WireWriter(output, { throw AssertionError(it) })
        writer.offer(Outbound(JSONObject().put("type", "hello")))
        writer.start()
        val producers = (0..3).map { producer -> Thread {
            repeat(100) { index -> assertTrue(writer.offer(Outbound(JSONObject().put("type", "video").put("producer", producer).put("index", index)))) }
        }.apply { start() } }
        producers.forEach { it.join() }
        writer.finish("stop", null)
        assertTrue(writer.awaitFinished(2000))
        val frames = events(output.toByteArray())
        assertEquals(402, frames.size)
        frames.forEachIndexed { index, packet -> assertEquals(index.toLong(), packet.first.getLong("seq")) }
        for (producer in 0..3) assertEquals((0 until 100).toList(), frames.filter { it.first.optInt("producer", -1) == producer }.map { it.first.getInt("index") })
    }

    @Test fun writerFailureNotifiesOnceAndStopsIntake() {
        val failures = Collections.synchronizedList(mutableListOf<Throwable>())
        val notified = CountDownLatch(1)
        val writer = WireWriter(object : OutputStream() {
            override fun write(value: Int) { throw IOException("broken transport") }
        }, { failures += it; notified.countDown() })
        writer.offer(Outbound(JSONObject().put("type", "hello")))
        writer.start()
        assertTrue(notified.await(2, TimeUnit.SECONDS))
        assertFalse(writer.offer(Outbound(JSONObject().put("type", "video"))))
        assertEquals(1, failures.size)
    }

    @Test fun fragmentedReaderPreservesUtf8AndRejectsBinaryCommands() {
        val raw = ByteArrayOutputStream()
        DataOutputStream(raw).apply {
            val header = JSONObject().put("v", 1).put("type", "start").put("note", "相机").toString().toByteArray(Charsets.UTF_8)
            writeInt(header.size); writeInt(0); write(header)
        }
        val underlying = ByteArrayInputStream(raw.toByteArray())
        val fragmented = object : InputStream() {
            override fun read(): Int = underlying.read()
            override fun read(bytes: ByteArray, offset: Int, length: Int): Int = underlying.read(bytes, offset, minOf(1, length))
        }
        assertEquals("相机", WireReader(fragmented).read().getString("note"))
        val bad = ByteArrayOutputStream()
        DataOutputStream(bad).apply { writeInt(1); writeInt(1) }
        try { WireReader(ByteArrayInputStream(bad.toByteArray())).read(); fail() } catch (_: IllegalArgumentException) { }
    }
}
