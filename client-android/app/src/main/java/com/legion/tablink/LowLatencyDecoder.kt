package com.legion.tablink

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

interface KeyframeRequester
{
    fun requestKeyframe()
}

class EncodedFrame(val payload: ByteArray, val keyframe: Boolean)

class LowLatencyDecoder(private val keyframeRequester: KeyframeRequester)
{
    companion object
    {
        const val QUEUE_DEPTH = 4
        const val INPUT_POLL_TIMEOUT_US = 5000
        const val OUTPUT_POLL_TIMEOUT_US = 500
        const val FRAME_POLL_TIMEOUT_MS = 2
        const val MAX_INPUT_SIZE = 4 * 1024 * 1024
        const val NAL_START_CODE_MIN = 3
        const val HEVC_NAL_TYPE_VPS = 32
        const val HEVC_NAL_TYPE_SPS = 33
        const val HEVC_NAL_TYPE_PPS = 34
        const val VENDOR_QTI_DEC_LOW_LATENCY = "vendor.qti-ext-dec-low-latency.enable"
        const val CSD_BUFFER_KEY = "csd-0"
        val CSD_START_CODE_4 = byteArrayOf(0, 0, 0, 1)
    }

    private val running = AtomicBoolean(false)
    private val queue = ArrayBlockingQueue<EncodedFrame>(QUEUE_DEPTH)
    private var surface: Surface? = null
    private var streamWidth = 0
    private var streamHeight = 0
    private var streamCodec = 0
    private var codec: MediaCodec? = null
    private var codecThread: Thread? = null
    private var awaitingKeyframe = true

    fun attachSurface(value: Surface?)
    {
        Log.d("LTL-DEC", "attachSurface: ${value != null}, valid=${value?.isValid}")
        surface = value
        if (value == null)
        {
            restartPipeline()
        }
    }

    fun onStreamConfigured(width: Int, height: Int, codecId: Int)
    {
        Log.d("LTL-DEC", "onStreamConfigured: ${width}x${height} codec=$codecId running=${running.get()}")
        streamWidth = width
        streamHeight = height
        streamCodec = codecId
        restartPipeline()
        startIfNecessary()
    }

    private var pushCount = 0
    fun pushEncodedFrame(payload: ByteArray, keyframe: Boolean)
    {
        pushCount++
        if (pushCount <= 5 || pushCount % 100 == 0)
        {
            Log.d("LTL-DEC", "pushEncodedFrame #$pushCount: ${payload.size}b keyframe=$keyframe running=${running.get()} queueSize=${queue.size}")
        }
        if (!running.get())
        {
            if (pushCount <= 5) Log.w("LTL-DEC", "pushEncodedFrame: NOT running, dropping frame")
            return
        }
        if (!queue.offer(EncodedFrame(payload, keyframe)))
        {
            // Queue full — stale frames cause latency. Clear and wait for
            // a clean IDR so the decoder doesn't ghost from broken refs.
            // At 30fps + queue 4, IDR arrives in ~66ms — imperceptible.
            queue.clear()
            awaitingKeyframe = true
            keyframeRequester.requestKeyframe()
        }
    }

    fun shutdown()
    {
        running.set(false)
        val thread = codecThread
        if (thread != null)
        {
            try
            {
                thread.join(2500)
            }
            catch (ignored: InterruptedException)
            {
            }
        }
        codecThread = null
        queue.clear()
        releaseCodec()
        surface = null
    }

    private fun startIfNecessary()
    {
        if (!running.compareAndSet(false, true))
        {
            Log.d("LTL-DEC", "startIfNecessary: already running")
            return
        }
        Log.d("LTL-DEC", "startIfNecessary: starting codec thread")
        awaitingKeyframe = true
        val thread = Thread(object : Runnable
        {
            override fun run()
            {
                runCodecLoop()
            }
        }, "ltl-decode")
        thread.isDaemon = true
        codecThread = thread
        thread.start()
    }

    private fun restartPipeline()
    {
        queue.clear()
        awaitingKeyframe = true
        releaseCodec()
    }

    private fun releaseCodec()
    {
        val current = codec
        codec = null
        if (current == null)
        {
            return
        }
        try
        {
            current.stop()
        }
        catch (ignored: IllegalStateException)
        {
        }
        try
        {
            current.release()
        }
        catch (ignored: IllegalStateException)
        {
        }
    }

    private var loopLogCount = 0
    private fun runCodecLoop()
    {
        Log.d("LTL-DEC", "runCodecLoop: STARTED surface=${surface != null} codec=$streamCodec")
        while (running.get())
        {
            val attachedSurface = surface
            if (attachedSurface == null || !attachedSurface.isValid || streamCodec != WireProtocol.CodecHevc)
            {
                loopLogCount++
                if (loopLogCount <= 3 || loopLogCount % 500 == 0)
                {
                    Log.w("LTL-DEC", "runCodecLoop waiting #$loopLogCount: surface=${attachedSurface != null} valid=${attachedSurface?.isValid} codec=$streamCodec expected=${WireProtocol.CodecHevc}")
                }
                sleepQuietly(10)
                continue
            }
            try
            {
                pumpQueue(attachedSurface)
                flushOutputs()
            }
            catch (exception: IllegalArgumentException)
            {
                Log.e("LTL-DEC", "runCodecLoop IllegalArgumentException", exception)
                restartPipeline()
                sleepQuietly(20)
            }
            catch (exception: IllegalStateException)
            {
                Log.e("LTL-DEC", "runCodecLoop IllegalStateException", exception)
                restartPipeline()
                sleepQuietly(20)
            }
        }
        Log.d("LTL-DEC", "runCodecLoop: EXITED")
        releaseCodec()
    }

    private var pumpCount = 0
    private fun pumpQueue(attachedSurface: Surface)
    {
        val frame = pollFrameIfKeyframe()
        if (frame == null)
        {
            return
        }
        pumpCount++
        if (pumpCount <= 10 || pumpCount % 100 == 0)
        {
            Log.d("LTL-DEC", "pumpQueue #$pumpCount: keyframe=${frame.keyframe} size=${frame.payload.size} codec=${codec != null}")
        }
        if (codec == null && frame.keyframe)
        {
            val parameterSets = extractParameterSets(frame.payload)
            if (parameterSets == null)
            {
                Log.e("LTL-DEC", "pumpQueue: extractParameterSets returned null! payload first16=${frame.payload.take(16).joinToString(" ") { String.format("%02X", it) }}")
                return
            }
            Log.d("LTL-DEC", "pumpQueue: got CSD ${parameterSets.size}b, creating codec for ${streamWidth}x${streamHeight}")
            ensureCodec(attachedSurface, parameterSets)
            if (codec == null)
            {
                Log.e("LTL-DEC", "pumpQueue: ensureCodec failed, codec still null")
                return
            }
            Log.d("LTL-DEC", "pumpQueue: codec created successfully!")
        }
        submitInput(frame.payload, frame.keyframe)
    }

    private fun pollFrameIfKeyframe(): EncodedFrame?
    {
        if (!awaitingKeyframe)
        {
            return queue.poll(FRAME_POLL_TIMEOUT_MS.toLong(), TimeUnit.MILLISECONDS)
        }
        val frame = queue.poll(FRAME_POLL_TIMEOUT_MS.toLong(), TimeUnit.MILLISECONDS) ?: return null
        if (frame.keyframe)
        {
            awaitingKeyframe = false
            return frame
        }
        while (true)
        {
            val skip = queue.poll() ?: break
            if (skip.keyframe)
            {
                awaitingKeyframe = false
                return skip
            }
        }
        return null
    }

    private fun ensureCodec(attachedSurface: Surface, parameterSets: ByteArray)
    {
        Log.d("LTL-DEC", "ensureCodec: creating HEVC decoder ${streamWidth}x${streamHeight} surface.valid=${attachedSurface.isValid}")
        try
        {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_HEVC, streamWidth, streamHeight)
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_INPUT_SIZE)
            format.setInteger(MediaFormat.KEY_PRIORITY, 0)
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
            format.setByteBuffer(CSD_BUFFER_KEY, ByteBuffer.wrap(parameterSets))
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            try { format.setInteger(VENDOR_QTI_DEC_LOW_LATENCY, 1) } catch (e: Exception) { Log.w("LTL-DEC", "QTI low-latency key not supported: ${e.message}") }

            val decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_HEVC)
            Log.d("LTL-DEC", "ensureCodec: decoder created, configuring...")
            decoder.configure(format, attachedSurface, null, 0)
            Log.d("LTL-DEC", "ensureCodec: configured, starting...")
            decoder.start()
            Log.d("LTL-DEC", "ensureCodec: STARTED OK")
            codec = decoder
            awaitingKeyframe = false
        }
        catch (e: Exception)
        {
            Log.e("LTL-DEC", "ensureCodec FAILED", e)
            codec = null
        }
    }

    private var submitCount = 0
    private fun submitInput(payload: ByteArray, keyframe: Boolean)
    {
        val current = codec ?: return
        val index = current.dequeueInputBuffer(INPUT_POLL_TIMEOUT_US.toLong())
        if (index < 0)
        {
            submitCount++
            if (submitCount <= 3) Log.w("LTL-DEC", "submitInput: no input buffer available")
            return
        }
        val inputBuffer = current.getInputBuffer(index) ?: return
        inputBuffer.clear()
        inputBuffer.limit(payload.size)
        inputBuffer.put(payload)
        val flags = if (keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
        current.queueInputBuffer(index, 0, payload.size, System.nanoTime() / 1000, flags)
        submitCount++
        if (submitCount <= 5 || submitCount % 100 == 0)
        {
            Log.d("LTL-DEC", "submitInput #$submitCount: ${payload.size}b keyframe=$keyframe bufIdx=$index")
        }
    }

    private var outputCount = 0
    private fun flushOutputs()
    {
        val current = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (running.get())
        {
            val index = current.dequeueOutputBuffer(info, OUTPUT_POLL_TIMEOUT_US.toLong())
            if (index < 0)
            {
                return
            }
            current.releaseOutputBuffer(index, true)
            outputCount++
            if (outputCount <= 5 || outputCount % 100 == 0)
            {
                Log.d("LTL-DEC", "flushOutputs #$outputCount: rendered frame bufIdx=$index size=${info.size}")
            }
        }
    }

    private fun extractParameterSets(payload: ByteArray): ByteArray?
    {
        val nalStarts = collectNalStarts(payload)
        if (nalStarts.isEmpty())
        {
            return null
        }

        val parameterSets = ByteArrayOutputStream()
        var found = 0
        for (position in nalStarts.indices)
        {
            val start = nalStarts[position]
            val end = if (position + 1 < nalStarts.size) nalStarts[position + 1] else payload.size
            val headerOffset = nalHeaderOffset(payload, start)
            if (headerOffset >= payload.size || headerOffset >= end)
            {
                continue
            }
            val firstByte = payload[headerOffset].toInt() and 0xFF
            val nalType = (firstByte shr 1) and 0x3F
            if (nalType != HEVC_NAL_TYPE_VPS && nalType != HEVC_NAL_TYPE_SPS && nalType != HEVC_NAL_TYPE_PPS)
            {
                continue
            }
            parameterSets.write(CSD_START_CODE_4, 0, CSD_START_CODE_4.size)
            parameterSets.write(payload, headerOffset, end - headerOffset)
            found += 1
        }
        if (found == 0)
        {
            return null
        }
        return parameterSets.toByteArray()
    }

    private fun collectNalStarts(payload: ByteArray): List<Int>
    {
        val starts = ArrayList<Int>()
        var scan = 0
        while (scan + NAL_START_CODE_MIN < payload.size)
        {
            if (payload[scan] == 0.toByte() && payload[scan + 1] == 0.toByte() && payload[scan + 2] == 1.toByte())
            {
                var begin = scan
                while (begin > 0 && payload[begin - 1] == 0.toByte())
                {
                    begin -= 1
                }
                starts.add(begin)
                scan += NAL_START_CODE_MIN
            }
            else
            {
                scan += 1
            }
        }
        return starts
    }

    private fun nalHeaderOffset(payload: ByteArray, start: Int): Int
    {
        if (start + 4 <= payload.size
            && payload[start] == 0.toByte()
            && payload[start + 1] == 0.toByte()
            && payload[start + 2] == 0.toByte()
            && payload[start + 3] == 1.toByte())
        {
            return start + 4
        }
        return start + NAL_START_CODE_MIN
    }

    private fun sleepQuietly(milliseconds: Long)
    {
        try
        {
            Thread.sleep(milliseconds)
        }
        catch (ignored: InterruptedException)
        {
        }
    }
}
