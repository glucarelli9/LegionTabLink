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

    private fun showConnecting()
    {
        welcomeOverlay.visibility = View.VISIBLE
        captureView.visibility = View.INVISIBLE
        statusText.text = getString(R.string.status_connecting)
        setDotColor(getColor(R.color.dot_connecting))
        startPulse()
    }

    private fun showConnected()
    {
        statusText.text = getString(R.string.status_connected)
        setDotColor(getColor(R.color.dot_connected))
        pulseAnimator?.cancel()
        statusDot.alpha = 1f
        // Brief delay then show the stream
        captureView.postDelayed({
            welcomeOverlay.visibility = View.GONE
            captureView.visibility = View.VISIBLE
        }, 600)
    }

    private fun showDisconnected()
    {
        welcomeOverlay.visibility = View.VISIBLE
        captureView.visibility = View.INVISIBLE
        statusText.text = getString(R.string.status_reconnecting)
        setDotColor(getColor(R.color.dot_connecting))
        startPulse()
    }

    private fun setDotColor(color: Int)
    {
        val bg = statusDot.background
        if (bg is GradientDrawable)
        {
            bg.setColor(color)
        }
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
