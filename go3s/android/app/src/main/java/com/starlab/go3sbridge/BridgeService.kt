package com.starlab.go3sbridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

class BridgeService : Service() {
    /** The camera connection belongs to the service; each desktop client has its own capture lifetime. */
    private class ClientState(val token: Long, val socket: Socket) {
        val ending = AtomicBoolean(false)
        val finished = CompletableDeferred<Unit>()
        lateinit var writer: WireWriter
        var startReceived = false
        var sessionJob: Job? = null
        var statusJob: Job? = null
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val closing = AtomicBoolean(false)
    private val ready = CompletableDeferred<Unit>()
    private var started = false
    @Volatile private var server: ServerSocket? = null
    @Volatile private var currentClient: ClientState? = null
    private var connectJob: Job? = null
    private lateinit var bridge: CameraBridge
    private lateinit var lifetime: BridgeLifetime
    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_DISCONNECT) {
            if (started) disconnectCamera(null) else stopSelf()
            return START_NOT_STICKY
        }
        if (started) { updateStatus(currentStatus); return START_NOT_STICKY }
        started = true
        startForeground(1, notification("正在启动…"))
        val power = getSystemService(PowerManager::class.java)
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Go3sBridge:connection").apply { acquire() }
        val wifi = applicationContext.getSystemService(WifiManager::class.java)
        @Suppress("DEPRECATION")
        wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "Go3sBridge:wifi").apply { acquire() }
        bridge = CameraBridge(application, ::updateStatus) { disconnectCamera(it) }
        lifetime = BridgeLifetime(bridge::stopPreview, bridge::release)
        connectJob = scope.launch {
            try {
                bridge.connect()
                ready.complete(Unit)
            } catch (error: Throwable) {
                ready.completeExceptionally(error)
                if (!closing.get()) disconnectCamera("连接失败：${error.message}")
            }
        }
        scope.launch { serveClients() }
        return START_NOT_STICKY
    }

    private fun notification(message: String): Notification {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "GO3S 采集连接", NotificationManager.IMPORTANCE_LOW))
        val content = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val disconnect = PendingIntent.getService(this, 1, Intent(this, BridgeService::class.java).setAction(ACTION_DISCONNECT), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("GO3S 采集桥").setContentText(message).setContentIntent(content)
            .addAction(Notification.Action.Builder(null, "断开相机", disconnect).build()).setOngoing(true).build()
    }

    private fun updateStatus(message: String) {
        currentStatus = message
        sendBroadcast(Intent(ACTION_STATUS).setPackage(packageName).putExtra("status", message))
        if (started && !closing.get()) getSystemService(NotificationManager::class.java).notify(1, notification(message))
    }

    /** Accept later explicit desktop clients only after the previous client has stopped and drained. */
    private suspend fun serveClients() {
        try {
            val listener = ServerSocket(18765, 1, InetAddress.getByName("127.0.0.1"))
            server = listener
            if (closing.get()) { listener.close(); return }
            while (!closing.get()) {
                val socket = listener.accept()
                socket.tcpNoDelay = true
                val client: ClientState
                synchronized(this) {
                    if (closing.get()) { socket.close(); return }
                    client = ClientState(lifetime.attachClient(), socket)
                    client.writer = WireWriter(socket.getOutputStream(), {
                        finishClient(client, "disconnect", "电脑连接断开：${it.message}")
                    })
                    client.writer.offer(Outbound(JSONObject().put("type", "hello")
                        .put("phone", JSONObject().put("model", Build.MODEL).put("android_sdk", Build.VERSION.SDK_INT).put("abis", JSONArray(Build.SUPPORTED_ABIS.toList())))
                        .put("sdk_version", "2.2.6").put("sdk_internal_loss_observable", false)))
                    currentClient = client
                    client.writer.start()
                }
                readClient(client)
                client.finished.await()
            }
        } catch (error: Throwable) {
            if (!closing.get()) disconnectCamera("电脑监听服务异常：${error.message}")
        }
    }

    private fun readClient(client: ClientState) {
        try {
            val incoming = WireReader(client.socket.getInputStream())
            while (!client.ending.get() && !closing.get()) {
                val command = incoming.read()
                when (command.getString("type")) {
                    "start" -> {
                        require(!client.startReceived) { "每个连接只允许一次 start" }
                        client.startReceived = true
                        startSession(client, command)
                    }
                    "stop" -> { finishClient(client, "stop", null); return }
                    else -> error("未知电脑请求")
                }
            }
        } catch (error: Throwable) {
            if (!client.ending.get()) finishClient(client, "disconnect", "电脑连接或协议错误：${error.message}")
        }
    }

    private fun isCurrent(client: ClientState): Boolean =
        currentClient === client && !client.ending.get() && !closing.get()

    private fun startSession(client: ClientState, command: JSONObject) {
        // Publish the job under the same lock as finishClient, so stop cannot miss an in-flight setup.
        synchronized(client) {
            if (!isCurrent(client)) return
            client.sessionJob = scope.launch {
                try {
                    val interval = command.getJSONObject("runtime").getDouble("status_interval_s")
                    require(interval.isFinite() && interval in 0.1..60.0) { "状态间隔需为 0.1–60 秒" }
                    val statusIntervalMs = (interval * 1000).toLong()
                    val imuEnabled = command.getJSONObject("stream").getBoolean("imu")
                    ready.await()
                    bridge.start(command, { offer(client, it) }, { finishClient(client, "error", it) }, { isCurrent(client) })
                    synchronized(client) {
                        if (isCurrent(client)) client.statusJob = scope.launch {
                            monitorSession(client, statusIntervalMs, imuEnabled)
                        }
                    }
                } catch (error: Throwable) {
                    if (isCurrent(client)) finishClient(client, "error", "启动采集失败：${error.message}")
                }
            }
        }
    }

    private suspend fun monitorSession(client: ClientState, statusIntervalMs: Long, imuEnabled: Boolean) {
        var nextStatusNs = 0L
        while (isCurrent(client)) {
            try {
                val now = SystemClock.elapsedRealtimeNanos()
                val videoReference = maxOf(bridge.lastVideoNs, bridge.streamStartNs)
                val gyroReference = maxOf(bridge.lastGyroNs, bridge.streamStartNs)
                require(now - videoReference < 10_000_000_000L) { "视频超过 10 秒无数据" }
                require(!imuEnabled || now - gyroReference < 10_000_000_000L) { "相机六轴 IMU 超过 10 秒无数据" }
                if (now >= nextStatusNs) {
                    offer(client, bridge.statusPacket())
                    nextStatusNs = now + statusIntervalMs * 1_000_000L
                }
                delay(minOf(statusIntervalMs, 500L))
            } catch (error: Throwable) {
                if (isCurrent(client)) finishClient(client, "error", "采集状态异常：${error.message}")
                return
            }
        }
    }

    private fun offer(client: ClientState, packet: Outbound) {
        if (!isCurrent(client)) return
        if (!client.writer.offer(packet)) finishClient(client, "error", "电脑传输队列已满（64 MiB），停止采集并写完已接收数据")
    }

    /** Stop this capture only. SDK heartbeat, Wi-Fi/wake locks and the listener remain active. */
    private fun finishClient(client: ClientState, reason: String, failure: String?) {
        synchronized(client) {
            if (!client.ending.compareAndSet(false, true)) return
            client.writer.stopIntake()
            client.sessionJob?.cancel()
            client.statusJob?.cancel()
        }
        runCatching { client.socket.shutdownInput() }
        if (currentClient === client) updateStatus(failure ?: "正在停止采集并传完已接收数据…")
        scope.launch {
            try {
                client.sessionJob?.cancelAndJoin()
                client.statusJob?.cancelAndJoin()
                lifetime.endClient(client.token)
                client.writer.finish(reason, failure)
                val complete = client.writer.awaitFinished(4500)
                if (!closing.get()) updateStatus(
                    if (complete && failure == null) "相机保持连接，等待电脑采集"
                    else "${if (complete) failure else "本次电脑传输不完整"}\n相机保持连接，等待电脑采集"
                )
            } finally {
                runCatching { client.socket.close() }
                synchronized(this@BridgeService) { if (currentClient === client) currentClient = null }
                client.finished.complete(Unit)
            }
        }
    }

    /** Only explicit phone disconnect, service destruction, or actual camera loss owns SDK release. */
    private fun disconnectCamera(failure: String?) {
        synchronized(this) {
            if (!closing.compareAndSet(false, true)) return
        }
        updateStatus(failure?.let { "$it\n相机连接已停止，请点击连接重新建立" } ?: "正在断开相机…")
        runCatching { server?.close() }
        connectJob?.cancel()
        val client = currentClient
        if (client != null) finishClient(client, if (failure == null) "stop" else "error", failure)
        scope.launch {
            try {
                connectJob?.cancelAndJoin()
                client?.finished?.await()
                if (::lifetime.isInitialized) lifetime.disconnectCamera()
                if (failure == null) updateStatus("已断开相机")
            } finally {
                releaseLocks()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                scope.cancel()
            }
        }
    }

    private fun releaseLocks() {
        wifiLock?.let { if (it.isHeld) it.release() }
        wakeLock?.let { if (it.isHeld) it.release() }
    }

    override fun onDestroy() {
        if (!closing.get()) disconnectCamera("手机服务停止")
        super.onDestroy()
    }

    companion object {
        const val ACTION_DISCONNECT = "com.starlab.go3sbridge.DISCONNECT"
        const val ACTION_STATUS = "com.starlab.go3sbridge.STATUS"
        private const val CHANNEL = "go3s_connection"
        @Volatile var currentStatus = "等待连接"
            private set
    }
}
