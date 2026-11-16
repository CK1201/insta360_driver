package com.starlab.go3sbridge

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class GyroTest {
    @Test fun sdkParserPreservesRawDeviceTimestampAndAllAxes() {
        val raw = ByteBuffer.allocate(112).order(ByteOrder.LITTLE_ENDIAN).apply {
            putLong(123456789L)
            listOf(1.0, -2.0, 3.5, -4.0, 5.25, -6.0).forEach { putDouble(it) }
            putLong(123456789L) // duplicate timestamps must remain intact
            listOf(7.0, 8.0, 9.0, 10.0, 11.0, 12.0).forEach { putDouble(it) }
        }.array()
        val original = raw.copyOf()
        val samples = CameraBridge.parseGyro(raw)
        assertEquals(2, samples.length())
        assertEquals(123456789L, samples.getJSONArray(0).getLong(0))
        assertEquals(123456789L, samples.getJSONArray(1).getLong(0))
        assertEquals(-2.0, samples.getJSONArray(0).getDouble(2), 0.0)
        assertEquals(12.0, samples.getJSONArray(1).getDouble(6), 0.0)
        assertArrayEquals(original, raw)
    }

    @Test fun partialSamplesAreRejected() {
        try { CameraBridge.parseGyro(ByteArray(55)); fail() } catch (_: IllegalArgumentException) { }
    }
}
