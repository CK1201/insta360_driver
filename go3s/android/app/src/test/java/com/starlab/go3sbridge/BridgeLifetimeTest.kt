package com.starlab.go3sbridge

import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream

class BridgeLifetimeTest {
    @Test fun endingDesktopCaptureKeepsCameraUntilExplicitDisconnect() = runBlocking {
        var previewStops = 0
        var cameraReleases = 0
        val lifetime = BridgeLifetime({ previewStops++ }, { cameraReleases++ })
        val first = lifetime.attachClient()
        assertTrue(lifetime.endClient(first))
        assertEquals(1, previewStops)
        assertEquals(0, cameraReleases)
        val second = lifetime.attachClient()
        assertFalse(lifetime.endClient(first)) // A stale timer must not stop the newer session.
        assertEquals(1, previewStops)
        assertTrue(lifetime.endClient(second))
        assertEquals(2, previewStops)
        assertEquals(0, cameraReleases)
        lifetime.disconnectCamera()
        lifetime.disconnectCamera()
        assertEquals(1, cameraReleases)
        try { lifetime.attachClient(); fail("released camera accepted a client") } catch (_: IllegalStateException) { }
    }

    @Test fun consecutiveClientsResetSequenceAndDrainBeforeKeepingCamera() {
        var previewStops = 0
        var cameraReleases = 0
        val lifetime = BridgeLifetime({ previewStops++ }, { cameraReleases++ })
        repeat(2) { capture ->
            val token = lifetime.attachClient()
            val output = ByteArrayOutputStream()
            val writer = WireWriter(output, { throw AssertionError(it) })
            assertTrue(writer.offer(Outbound(JSONObject().put("type", "hello"))))
            val raw = byteArrayOf(capture.toByte(), 7, -1, 0)
            assertTrue(writer.offer(Outbound(JSONObject().put("type", "video"), raw)))
            writer.stopIntake()
            assertTrue(lifetime.endClient(token))
            writer.finish("stop", null)
            writer.start()
            assertTrue(writer.awaitFinished(2000))
            val input = DataInputStream(ByteArrayInputStream(output.toByteArray()))
            val headers = mutableListOf<JSONObject>()
            val payloads = mutableListOf<ByteArray>()
            while (input.available() > 0) {
                val headerSize = input.readInt()
                val payloadSize = input.readInt()
                val header = ByteArray(headerSize).also { input.readFully(it) }
                headers += JSONObject(String(header, Charsets.UTF_8))
                payloads += ByteArray(payloadSize).also { input.readFully(it) }
            }
            assertEquals(listOf("hello", "video", "end"), headers.map { it.getString("type") })
            assertEquals(listOf(0L, 1L, 2L), headers.map { it.getLong("seq") })
            assertArrayEquals(raw, payloads[1])
            assertEquals("stop", headers.last().getString("reason"))
            assertEquals(0, cameraReleases)
        }
        assertEquals(2, previewStops)
    }
}
