package com.starlab.go3sbridge

import android.app.Application
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.SystemClock
import com.arashivision.onestream.Gyro.OneGyroField
import com.arashivision.onestream.ImageData
import com.arashivision.onestream.OneStreamPipeline
import com.arashivision.onestream.pipeline.ICameraLivePipline
import com.arashivision.onestream.pipeline.ICameraPreviewPipeline
import com.arashivision.onestream.pipeline.ICameraPreviewScreenCaptureCallback
import com.arashivision.sdk.camera.InstaCameraSDK
import com.arashivision.sdk.camera.api.CameraDevice
import com.arashivision.sdk.camera.api.param.listener.AuthorizationListener
import com.arashivision.sdk.camera.api.param.listener.DisconnectListener
import com.arashivision.sdk.camera.api.preview.CameraStreamListener
import com.arashivision.sdk.camera.api.preview.PreviewStreamFrame
import com.arashivision.sdk.camera.api.preview.PreviewStreamParamsUpdate
import com.arashivision.sdk.camera.api.preview.PreviewStreamType
import com.arashivision.sdk.camera.core.model.CameraType
import com.arashivision.sdk.camera.core.model.ConnectType
import com.arashivision.sdk.camera.core.model.FunctionMode
import com.arashivision.sdk.camera.core.model.authorization.AuthorizationOperationType
import com.arashivision.sdk.camera.core.model.authorization.AuthorizationResult
import com.arashivision.sdk.camera.core.model.authorization.AuthorizationStatus
import com.arashivision.sdk.camera.core.model.option.VideoEncode
import com.arashivision.sdk.common.log.LogLevel
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeout
import org.json.JSONArray
import org.json.JSONObject

internal class CameraBridge(
    private val app: Application,
    private val status: (String) -> Unit,
    private val cameraLost: (String) -> Unit,
) {
    private lateinit var camera: CameraDevice
    private class PreviewSession(
        val emit: (Outbound) -> Unit,
        val fail: (String) -> Unit,
        val isCurrentClient: () -> Boolean,
    ) {
        @Volatile var accepting = false
        lateinit var listener: CameraStreamListener
    }
    @Volatile private var activePreview: PreviewSession? = null
    @Volatile private var released = false
    private var previewInitialized = false
    @Volatile private var imu = true
    @Volatile private var codec = "h264"
    @Volatile private var width = 0
    @Volatile private var height = 0
    @Volatile private var fps = 0
    @Volatile var lastVideoNs = 0L
        private set
    @Volatile var lastGyroNs = 0L
        private set
    @Volatile var streamStartNs = 0L
        private set

    suspend fun connect() {
        InstaCameraSDK.init(app) { logLevel = LogLevel.ERROR }
        val manager = app.getSystemService(ConnectivityManager::class.java)
        val wifi = manager.allNetworks.filter {
            manager.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }.singleOrNull() ?: error("请先手动连接唯一 GO3S Wi-Fi 热点")
        camera = CameraDevice.get(ConnectType.WIFI)
        camera.registerDisconnectListener(object : DisconnectListener {
            override fun onDisconnect(throwable: Throwable?) {
                if (!released) cameraLost("相机连接断开：${throwable?.message ?: "device_lost"}")
            }
        })
        status("正在通过 Wi-Fi 连接相机…")
        withTimeout(30_000) { camera.connect(wifi.networkHandle).getOrThrow() }
        require(camera.system.fetchCameraType().getOrThrow() == CameraType.GO_3S) { "仅支持 GO3S，相机型号不符" }
        authorize()
        status("相机保持连接，等待电脑采集")
    }

    private suspend fun authorize() {
        val result = CompletableDeferred<AuthorizationResult>()
        val listener = object : AuthorizationListener {
            override fun onAuthorizationResult(operationType: AuthorizationOperationType, authorization: AuthorizationResult) {
                result.complete(authorization)
            }
        }
        camera.registerAuthorizationListener(listener)
        try {
            when (camera.checkAuthorization().getOrThrow()) {
                AuthorizationStatus.AUTHORIZED -> Unit
                AuthorizationStatus.SYSTEM_BUSY -> error("相机忙，无法授权；请停止其他相机操作")
                AuthorizationStatus.UNAUTHORIZED -> {
                    status("请在 GO3S 相机上确认授权（最多等待 60 秒）")
                    require(withTimeout(60_000) { result.await() } == AuthorizationResult.SUCCESS) { "相机授权被拒绝或超时" }
                }
            }
        } finally {
            camera.unregisterAuthorizationListener(listener)
        }
    }

    suspend fun start(request: JSONObject, emit: (Outbound) -> Unit, fail: (String) -> Unit, isCurrentClient: () -> Boolean) {
        check(!released && isCurrentClient()) { "采集已经停止" }
        check(activePreview == null) { "相机已有预览会话" }
        val requestedCamera = request.getJSONObject("camera")
        val requestedSerial = requestedCamera.getString("serial")
        val actualSerial = camera.system.fetchSerialNumber().getOrThrow()
        require(requestedSerial.isBlank() || requestedSerial == actualSerial) { "相机序列号不符，实际为 $actualSerial" }
        imu = request.getJSONObject("stream").getBoolean("imu")
        val requestedExposure = ExposureRequest.parse(requestedCamera)
        camera.capture.functionMode.setValue(FunctionMode.VIDEO_NORMAL).getOrThrow()
        require(camera.capture.functionMode.fetchValue().getOrThrow() == FunctionMode.VIDEO_NORMAL) { "相机普通视频模式回读不符" }
        val readback = applyAndReadExposure(camera.capture, requestedExposure)
        codec = when (camera.system.fetchVideoEncodeType().getOrThrow()) {
            VideoEncode.ENCODE_H264 -> "h264"
            VideoEncode.ENCODE_H265 -> "h265"
        }
        if (!previewInitialized) {
            camera.preview.init(app)
            previewInitialized = true
        }
        val session = PreviewSession(emit, fail, isCurrentClient)
        session.listener = streamListener(session)
        activePreview = session
        camera.preview.setPipeline(pipeline(session))
        camera.preview.registerCameraStreamListener(session.listener)
        val previewParams = camera.preview.getPreviewParams().getOrThrow()
        val params = if (previewParams.previewNum == 0) previewParams.firstStream else previewParams.secondStream
        width = params.width
        height = params.height
        fps = params.fps
        require(fps == 30) { "SDK 普通视频预览为 ${fps}fps，目标为 30fps；已停止，不尝试其他模式" }
        emit(Outbound(JSONObject().put("type", "session")
            .put("identity", JSONObject().put("model", "GO_3S").put("serial", actualSerial)
                .put("firmware", camera.system.fetchFirmwareRevision().getOrThrow()).put("sdk_version", "2.2.6"))
            .put("requested", request).put("readback", readback).put("preview", previewJson())
            .put("sdk_internal_loss_observable", false)))
        check(!released && isCurrentClient()) { "采集已经停止" }
        session.accepting = true
        lastVideoNs = 0L
        lastGyroNs = 0L
        streamStartNs = SystemClock.elapsedRealtimeNanos()
        camera.preview.startStream()
        status("正在采集 GO3S 视频和${if (imu) "相机六轴 IMU" else "曝光数据"}；电脑保存并显示画面")
    }

    fun previewJson(): JSONObject = JSONObject().put("codec", codec).put("width", width).put("height", height).put("fps", fps)

    suspend fun statusPacket(): Outbound {
        if (!camera.isConnected()) {
            cameraLost("相机连接已断开")
            error("相机已断开")
        }
        val battery = camera.system.fetchBatteryData().getOrThrow()
        val storage = camera.system.fetchStorageDataList().getOrThrow()
        return Outbound(JSONObject().put("type", "status").put("phone_receive_ns", SystemClock.elapsedRealtimeNanos()).put("connected", true)
            .put("battery_level", battery.level).put("battery_scale", battery.scale)
            .put("power", if (battery.isCharging) "charging" else "battery")
            .put("battery", JSONObject().put("isCharging", battery.isCharging).put("level", battery.level).put("scale", battery.scale)
                .put("type", battery.type.name).put("type_native", battery.type.nativeValue).put("temp", battery.temp))
            .put("storage", JSONArray().apply { storage.forEach { s -> put(JSONObject().put("state", s.state.name)
                .put("state_native", s.state.nativeValue).put("free", s.free).put("total", s.total)
                .put("fileLocation", s.fileLocation.name).put("fileLocation_native", s.fileLocation.nativeValue).put("spec", s.spec)) } })
            .put("preview", previewJson()))
    }

    fun stopPreview() {
        val session = activePreview
        session?.accepting = false
        activePreview = null
        if (::camera.isInitialized && previewInitialized) {
            runCatching { camera.preview.stopStream() }
            if (session != null) runCatching { camera.preview.unregisterCameraStreamListener(session.listener) }
            runCatching { camera.preview.setPipeline(null) }
        }
    }

    suspend fun release() {
        released = true
        stopPreview()
        if (::camera.isInitialized) {
            runCatching { withTimeout(2000) { camera.disconnect().getOrThrow() } }
            runCatching { camera.release() }
        }
    }

    private fun accepting(session: PreviewSession): Boolean =
        activePreview === session && session.accepting && !released && session.isCurrentClient()

    private fun streamListener(session: PreviewSession) = object : CameraStreamListener {
        override fun onOpening() = Unit
        override fun onOpened() = Unit
        override fun onIdle() { if (accepting(session)) session.fail("相机预览意外停止") }
        override fun onParamsChanged(paramsUpdate: PreviewStreamParamsUpdate) {
            if (activePreview !== session) return
            width = paramsUpdate.previewWidth
            height = paramsUpdate.previewHeight
            fps = paramsUpdate.previewFps
            if (accepting(session) && fps != 30) session.fail("相机实际预览变为 ${fps}fps，目标为 30fps")
        }
        override fun onStreamDataNotify(streamData: PreviewStreamFrame) {
            if (!accepting(session)) return
            val receive = SystemClock.elapsedRealtimeNanos()
            if (streamData.type.isVideo) {
                lastVideoNs = receive
                session.emit(Outbound(JSONObject().put("type", "video").put("camera_timestamp", streamData.timestamp)
                    .put("phone_receive_ns", receive).put("stream_index", 0).put("codec", codec), streamData.data.copyOf()))
            } else if (streamData.type == PreviewStreamType.GYRO && imu) {
                lastGyroNs = receive
                val raw = streamData.data.copyOf()
                val header = JSONObject().put("type", "gyro").put("camera_timestamp", streamData.timestamp).put("phone_receive_ns", receive)
                session.emit(Outbound(header, raw) {
                    try {
                        header.put("samples", parseGyro(raw))
                        require(header.toString().toByteArray(Charsets.UTF_8).size < WireWriter.MAX_HEADER - 128) { "IMU 消息头超过协议限制" }
                    }
                    catch (error: Throwable) {
                        // Preserve the accepted raw callback even if parsing fails; then terminate.
                        header.put("samples", JSONArray()).put("parse_error", error.message ?: "invalid gyro")
                        session.fail("相机 IMU 数据解析失败：${error.message}")
                    }
                })
            }
        }
    }

    private fun pipeline(session: PreviewSession) = object : ICameraPreviewPipeline {
        override fun onExposureStream(timestamp: Long, exposure: Double) {
            if (!accepting(session)) return
            if (!exposure.isFinite() || exposure <= 0.0) { session.fail("相机返回无效曝光时间"); return }
            session.emit(Outbound(JSONObject().put("type", "exposure").put("camera_timestamp", timestamp)
                .put("phone_receive_ns", SystemClock.elapsedRealtimeNanos()).put("exposure_time_s", exposure)))
        }
        override fun onVideoStream(images: Array<out ImageData>?) {
            // Header parameters only: no MediaCodec, phone rendering or video decoding.
            if (!accepting(session)) return
            images?.firstOrNull()?.let {
                width = it.width; height = it.height; fps = it.fps
                codec = if (it.mH265) "h265" else "h264"
                if (accepting(session) && fps > 0 && fps != 30) session.fail("相机码流头实际为 ${fps}fps，目标为 30fps")
            }
        }
        override fun onAudioStream(timestamp: Long, data: ByteArray?) = Unit
        override fun onGyroStream(data: ByteArray?, timestamp: Long) = Unit
        override fun onP3Algorithm(data: ByteArray?, timestamp: Long) = Unit
        override fun onUpdatePreviewDelta(delta: Long) = Unit
        override fun onStartLive(param: OneStreamPipeline.RecordParam?, pipeline: ICameraLivePipline?) = Unit
        override fun onStopLive() = Unit
        override fun onScreenCapture(width: Int, height: Int, path: String?, callback: ICameraPreviewScreenCaptureCallback?) = Unit
    }

    companion object {
        internal fun parseGyro(raw: ByteArray): JSONArray {
            require(raw.isNotEmpty() && raw.size % OneGyroField.ONE_GYRO_LEN == 0) { "IMU 负载长度不匹配" }
            return JSONArray().apply {
                for (offset in raw.indices step OneGyroField.ONE_GYRO_LEN) {
                    val sample = OneGyroField(raw, offset, 0L)
                    put(JSONArray().put(sample.originTsMs).apply {
                        sample.accels.forEach { put(it) }
                        sample.gyros.forEach { put(it) }
                    })
                }
            }
        }
    }
}
