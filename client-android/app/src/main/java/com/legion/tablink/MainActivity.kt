package com.legion.tablink

import android.animation.ObjectAnimator
import android.app.Activity
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.TextView

class MainActivity : Activity()
{
    private lateinit var captureView: LtlCaptureView
    private lateinit var welcomeOverlay: View
    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var hintText: TextView
    private lateinit var chipText: TextView
    private lateinit var chip: View
    private lateinit var scene: LinkSceneView
    private lateinit var glow: GlowBackgroundView
    private lateinit var decoder: LowLatencyDecoder
    private lateinit var inputSender: InputSender
    private lateinit var streamClient: StreamClient
    private var pulseAnimator: ObjectAnimator? = null

    private val surfaceLifecycle = object : SurfaceHolder.Callback
    {
        override fun surfaceCreated(holder: SurfaceHolder)
        {
            Log.d("LTL-MAIN", "surfaceCreated: valid=${holder.surface.isValid}")
            decoder.attachSurface(holder.surface)
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int)
        {
            Log.d("LTL-MAIN", "surfaceChanged: ${width}x${height} format=$format")
        }

        override fun surfaceDestroyed(holder: SurfaceHolder)
        {
            Log.d("LTL-MAIN", "surfaceDestroyed")
            decoder.attachSurface(null)
        }
    }

    private val keyframeRequester = object : KeyframeRequester
    {
        override fun requestKeyframe()
        {
            inputSender.requestKeyframe()
        }
    }

    private val connectionCallback = object : ConnectionListener
    {
        override fun onConnecting()
        {
            runOnUiThread { showConnecting() }
        }

        override fun onConnected(width: Int, height: Int, fps: Int)
        {
            runOnUiThread { showConnected() }
        }

        override fun onDisconnected()
        {
            runOnUiThread { showDisconnected() }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?)
    {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        captureView = findViewById(R.id.captureView)
        welcomeOverlay = findViewById(R.id.welcomeOverlay)
        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        hintText = findViewById(R.id.hintText)
        chipText = findViewById(R.id.chipText)
        chip = findViewById(R.id.chip)
        scene = findViewById(R.id.scene)
        glow = findViewById(R.id.glow)

        if (resources.configuration.screenWidthDp < 600)
        {
            findViewById<View>(R.id.column).layoutParams.width = android.view.ViewGroup.LayoutParams.MATCH_PARENT
        }
        applyState(getString(R.string.status_connecting), connected = false)

        val metrics = resources.displayMetrics
        val tabletWidth = metrics.widthPixels.coerceAtLeast(1)
        val tabletHeight = metrics.heightPixels.coerceAtLeast(1)

        decoder = LowLatencyDecoder(keyframeRequester)
        inputSender = InputSender()
        streamClient = StreamClient(decoder, tabletWidth, tabletHeight)
        streamClient.connectionListener = connectionCallback

        captureView.inputSender = inputSender
        captureView.holder.addCallback(surfaceLifecycle)

        applyImmersiveChrome()
        startPulse()
    }

    override fun onResume()
    {
        super.onResume()
        applyImmersiveChrome()
        inputSender.start()
        streamClient.start()
    }

    override fun onPause()
    {
        streamClient.stop()
        inputSender.stop()
        decoder.shutdown()
        super.onPause()
    }

    override fun onDestroy()
    {
        pulseAnimator?.cancel()
        streamClient.stop()
        inputSender.stop()
        decoder.shutdown()
        super.onDestroy()
    }

    private var wasConnected = false

    private fun showConnecting()
    {
        welcomeOverlay.visibility = View.VISIBLE
        captureView.visibility = View.INVISIBLE
        val text = if (wasConnected) R.string.status_reconnecting else R.string.status_connecting
        applyState(getString(text), connected = false)
        startPulse()
    }

    private fun showConnected()
    {
        wasConnected = true
        applyState(getString(R.string.status_connected), connected = true)
        pulseAnimator?.cancel()
        statusDot.alpha = 1f
        // Let the connect animation play, then reveal the stream
        captureView.postDelayed({
            welcomeOverlay.visibility = View.GONE
            captureView.visibility = View.VISIBLE
        }, 900)
    }

    private fun showDisconnected()
    {
        welcomeOverlay.visibility = View.VISIBLE
        captureView.visibility = View.INVISIBLE
        val text = if (wasConnected) R.string.status_reconnecting else R.string.status_connecting
        applyState(getString(text), connected = false)
        startPulse()
    }

    private var currentConnected: Boolean? = null

    private fun applyState(status: String, connected: Boolean)
    {
        statusText.text = status
        if (currentConnected == connected) return
        currentConnected = connected

        val color = getColor(if (connected) R.color.dot_connected else R.color.dot_connecting)
        chipText.text = getString(if (connected) R.string.chip_connected else R.string.chip_connecting)
        chipText.setTextColor(blendWithWhite(color, 0.3f))
        hintText.text = getString(if (connected) R.string.hint_connected else R.string.hint_connecting)
        (statusDot.background.mutate() as? GradientDrawable)?.setColor(color)
        (chip.background.mutate() as? GradientDrawable)?.apply {
            setColor(Palette.withAlpha(color, 0.12f))
            setStroke((resources.displayMetrics.density).toInt().coerceAtLeast(1), Palette.withAlpha(color, 0.30f))
        }
        scene.setConnected(connected)
        glow.setConnected(connected)
    }

    private fun blendWithWhite(color: Int, amount: Float): Int
    {
        fun ch(c: Int) = (c + (255 - c) * amount).toInt()
        return android.graphics.Color.rgb(ch(android.graphics.Color.red(color)),
            ch(android.graphics.Color.green(color)), ch(android.graphics.Color.blue(color)))
    }

    private fun startPulse()
    {
        pulseAnimator?.cancel()
        val anim = ObjectAnimator.ofFloat(statusDot, "alpha", 1f, 0.3f)
        anim.duration = 800
        anim.repeatMode = ObjectAnimator.REVERSE
        anim.repeatCount = ObjectAnimator.INFINITE
        anim.start()
        pulseAnimator = anim
    }

    private fun applyImmersiveChrome()
    {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val insetsController: WindowInsetsController? = window.insetsController
        if (insetsController != null)
        {
            insetsController.hide(WindowInsets.Type.systemBars())
            insetsController.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
}
