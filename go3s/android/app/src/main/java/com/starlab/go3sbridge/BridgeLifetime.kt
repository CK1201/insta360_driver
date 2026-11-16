package com.starlab.go3sbridge

/** Camera ownership outlives each desktop capture. Old client tokens cannot stop a newer capture. */
internal class BridgeLifetime(
    private val stopPreview: () -> Unit,
    private val releaseCamera: suspend () -> Unit,
) {
    private var nextClient = 0L
    private var activeClient: Long? = null
    private var released = false

    @Synchronized fun attachClient(): Long {
        check(!released) { "相机连接已关闭" }
        check(activeClient == null) { "已有电脑采集连接" }
        return nextClient++.also { activeClient = it }
    }

    fun endClient(token: Long): Boolean {
        synchronized(this) {
            if (released || activeClient != token) return false
            activeClient = null
        }
        stopPreview()
        return true
    }

    suspend fun disconnectCamera() {
        synchronized(this) {
            if (released) return
            released = true
            activeClient = null
        }
        stopPreview()
        releaseCamera()
    }
}
