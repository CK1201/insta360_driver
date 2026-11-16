package com.starlab.go3sbridge

import android.Manifest
import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity() {
    private lateinit var status: TextView
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            status.text = intent.getStringExtra("status") ?: "等待连接"
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val padding = (20 * resources.displayMetrics.density).toInt()
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding, padding, padding)
        }
        layout.addView(TextView(this).apply {
            textSize = 24f
            text = "GO3S 采集桥"
        })
        layout.addView(TextView(this).apply {
            textSize = 16f
            text = "先在手机 Wi-Fi 设置中连接 GO3S 热点，再用 USB 连接电脑。\n点击连接后，在相机上确认授权；随后在电脑启动采集。\n电脑采集结束后相机保持连接；需要断开时点击“断开相机”。"
        })
        layout.addView(Button(this).apply {
            text = "连接"
            setOnClickListener {
                if (requestNeededPermissions()) {
                    startForegroundService(Intent(this@MainActivity, BridgeService::class.java))
                }
            }
        })
        layout.addView(Button(this).apply {
            text = "断开相机"
            setOnClickListener {
                startService(Intent(this@MainActivity, BridgeService::class.java).setAction(BridgeService.ACTION_DISCONNECT))
            }
        })
        status = TextView(this).apply {
            textSize = 16f
            text = BridgeService.currentStatus
        }
        layout.addView(status)
        setContentView(layout)
    }

    override fun onStart() {
        super.onStart()
        val filter = IntentFilter(BridgeService.ACTION_STATUS)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, RECEIVER_NOT_EXPORTED)
        else @Suppress("DEPRECATION") registerReceiver(receiver, filter)
        status.text = BridgeService.currentStatus
    }

    override fun onStop() {
        unregisterReceiver(receiver)
        super.onStop()
    }

    private fun requestNeededPermissions(): Boolean {
        val required = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) {
            required += Manifest.permission.NEARBY_WIFI_DEVICES
            required += Manifest.permission.POST_NOTIFICATIONS
        }
        val missing = required.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isEmpty()) return true
        requestPermissions(missing.toTypedArray(), 10)
        status.text = "请允许权限，然后再次点击连接。"
        return false
    }
}
