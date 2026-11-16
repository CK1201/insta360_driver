package com.starlab.go3sbridge

import com.arashivision.sdk.camera.core.model.photography.ExposureMode
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ExposureSettingsTest {
    @Test fun secondsUseNumeratorDenominatorAndAutoZeroPair() {
        assertEquals(0.0 to 0.0, secondsToFraction(0.0))
        assertEquals(1.0 to 120.0, secondsToFraction(1.0 / 120.0))
        assertEquals(2.5 to 1.0, secondsToFraction(2.5))
        assertEquals(0.08, shutterSeconds(1.0 to 12.5), 1e-12)
    }

    @Test fun go3sManualTabCanRepresentAllFourOrdinaryPrograms() {
        assertEquals("auto", semanticExposureMode(ExposureMode.MANUAL, 0, 0.0))
        assertEquals("iso_priority", semanticExposureMode(ExposureMode.MANUAL, 100, 0.0))
        assertEquals("shutter_priority", semanticExposureMode(ExposureMode.MANUAL, 0, 1.0 / 120))
        assertEquals("manual", semanticExposureMode(ExposureMode.MANUAL, 100, 1.0 / 120))
        try { semanticExposureMode(ExposureMode.AUTO, 0, 0.0); fail() } catch (_: IllegalArgumentException) { }
    }

    @Test fun rejectUnknownModesAndInconsistentPrioritySettings() {
        val camera = JSONObject("""{"exposure":{"mode":"full_auto","iso":null,"shutter_s":null,"ev":0},"white_balance_kelvin":0}""")
        try { ExposureRequest.parse(camera); fail() } catch (_: IllegalArgumentException) { }
        camera.getJSONObject("exposure").put("mode", "iso_priority").put("iso", 100).put("shutter_s", 0.01)
        try { ExposureRequest.parse(camera); fail() } catch (_: IllegalArgumentException) { }
        camera.getJSONObject("exposure").put("shutter_s", JSONObject.NULL)
        val parsed = ExposureRequest.parse(camera)
        assertEquals("iso_priority", parsed.mode)
        assertEquals(100, parsed.iso)
        assertEquals(0.0, parsed.ev, 0.0)
    }
}
