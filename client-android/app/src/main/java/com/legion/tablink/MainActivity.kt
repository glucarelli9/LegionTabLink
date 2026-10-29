package com.legion.tablink

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager

class MainActivity : Activity(){
    private lateinit var captureView: LtlCaptureView
    private lateinit var decoder: LowLatencyDecoder
    private lateinit var inputSender: InputSender
    private lateinit var streamClient: StreamClient

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

    override fun onCreate(savedInstanceState: Bundle?)
    {
        super.onCreate(savedInstanceState)

        val metrics = resources.displayMetrics
        val tabletWidth = metrics.widthPixels.coerceAtLeast(1)
        val tabletHeight = metrics.heightPixels.coerceAtLeast(1)

        decoder = LowLatencyDecoder(keyframeRequester)
        inputSender = InputSender()
        streamClient = StreamClient(decoder, tabletWidth, tabletHeight)

        captureView = LtlCaptureView(this, inputSender)
        captureView.holder.addCallback(surfaceLifecycle)
        setContentView(captureView, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        applyImmersiveChrome()
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
        streamClient.stop()
        inputSender.stop()
        decoder.shutdown()
        super.onDestroy()
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
