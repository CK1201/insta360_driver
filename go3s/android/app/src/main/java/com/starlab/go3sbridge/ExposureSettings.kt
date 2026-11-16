package com.starlab.go3sbridge

import com.arashivision.sdk.camera.api.CameraCapture
import com.arashivision.sdk.camera.core.model.photography.ExposureMode
import org.json.JSONObject
import kotlin.math.abs

internal data class ExposureRequest(val mode: String, val iso: Int, val shutter: Double, val ev: Double, val whiteBalance: Int) {
    companion object {
        fun parse(camera: JSONObject): ExposureRequest {
            val exposure = camera.getJSONObject("exposure")
            val mode = exposure.getString("mode")
            require(mode in setOf("auto", "manual", "iso_priority", "shutter_priority")) { "不支持的曝光模式：$mode" }
            val iso = if (exposure.isNull("iso")) 0 else exposure.getInt("iso")
            val shutter = if (exposure.isNull("shutter_s")) 0.0 else exposure.getDouble("shutter_s")
            val ev = exposure.getDouble("ev")
            val wb = camera.getInt("white_balance_kelvin")
            require(iso >= 0 && shutter.isFinite() && shutter >= 0 && ev.isFinite() && wb >= 0) { "曝光或白平衡参数无效" }
            require(when (mode) {
                "auto" -> iso == 0 && shutter == 0.0
                "manual" -> iso > 0 && shutter > 0.0
                "iso_priority" -> iso > 0 && shutter == 0.0
                else -> iso == 0 && shutter > 0.0
            }) { "曝光模式与 ISO / 快门组合不一致" }
            return ExposureRequest(mode, iso, shutter, ev, wb)
        }
    }
}

internal fun shutterSeconds(pair: Pair<Double, Double>): Double {
    if (pair == (0.0 to 0.0)) return 0.0
    require(pair.first > 0.0 && pair.second > 0.0 && pair.first.isFinite() && pair.second.isFinite()) { "相机返回无效快门" }
    return pair.first / pair.second
}

internal fun secondsToFraction(seconds: Double): Pair<Double, Double> {
    require(seconds.isFinite() && seconds >= 0.0)
    return when {
        seconds == 0.0 -> 0.0 to 0.0
        seconds <= 1.0 -> 1.0 to (1.0 / seconds)
        else -> seconds to 1.0
    }
}

internal fun closeNumber(first: Double, second: Double) = abs(first - second) <= maxOf(1e-9, abs(second) * 1e-6)

/** GO3S EXPOSURE_FIRST maps all four ordinary exposure programs into SDK MANUAL. */
internal fun semanticExposureMode(sdkMode: ExposureMode, iso: Int, shutter: Double): String {
    require(sdkMode == ExposureMode.MANUAL) { "相机为 FULL_AUTO 或独立曝光；无法严格确认普通曝光配置" }
    return when {
        iso == 0 && shutter == 0.0 -> "auto"
        iso > 0 && shutter == 0.0 -> "iso_priority"
        iso == 0 && shutter > 0.0 -> "shutter_priority"
        iso > 0 && shutter > 0.0 -> "manual"
        else -> error("相机返回无效 ISO / 快门组合")
    }
}

internal suspend fun applyAndReadExposure(capture: CameraCapture, requested: ExposureRequest): JSONObject {
    require(ExposureMode.MANUAL in capture.exposureProgram.getSupported().getOrThrow()) { "相机不支持普通曝光控制" }
    require(requested.iso in capture.exposureISO.getSupported().getOrThrow()) { "相机不支持 ISO ${requested.iso}" }
    val supportedShutter = capture.exposureShutterSpeed.getSupported().getOrThrow()
    val fraction = secondsToFraction(requested.shutter)
    require(supportedShutter.any { closeNumber(shutterSeconds(it), shutterSeconds(fraction)) }) { "相机不支持快门 ${requested.shutter} 秒" }
    require(capture.exposureBias.getSupported().getOrThrow().any { closeNumber(it, requested.ev) }) { "相机不支持 EV ${requested.ev}" }
    require(requested.whiteBalance in capture.whiteBalance.getSupported().getOrThrow()) { "相机不支持白平衡 ${requested.whiteBalance}" }
    capture.exposureProgram.setValue(ExposureMode.MANUAL).getOrThrow()
    capture.exposureISO.setValue(requested.iso).getOrThrow()
    capture.exposureShutterSpeed.setValue(fraction).getOrThrow()
    capture.exposureBias.setValue(requested.ev).getOrThrow()
    capture.whiteBalance.setValue(requested.whiteBalance).getOrThrow()
    val sdkMode = capture.exposureProgram.fetchValue().getOrThrow()
    val iso = capture.exposureISO.fetchValue().getOrThrow()
    val shutter = shutterSeconds(capture.exposureShutterSpeed.fetchValue().getOrThrow())
    val ev = capture.exposureBias.fetchValue().getOrThrow()
    val whiteBalance = capture.whiteBalance.fetchValue().getOrThrow()
    val mode = semanticExposureMode(sdkMode, iso, shutter)
    require(mode == requested.mode && iso == requested.iso && closeNumber(shutter, requested.shutter) && closeNumber(ev, requested.ev) && whiteBalance == requested.whiteBalance) {
        "参数回读不一致：mode=$mode ISO=$iso shutter=$shutter EV=$ev WB=$whiteBalance"
    }
    return JSONObject().put("exposure", JSONObject().put("mode", mode).put("iso", iso).put("shutter_s", shutter).put("ev", ev).put("sdk_exposure_mode", sdkMode.name))
        .put("white_balance_kelvin", whiteBalance)
}
