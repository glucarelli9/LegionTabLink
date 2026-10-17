package com.legion.tablink

import android.annotation.SuppressLint
import android.view.MotionEvent
import android.view.View
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

data class LtlPointerSample(
    val pointerId: Int,
    val x: Float,
    val y: Float,
    val pressure: Float,
    val tiltXDegrees: Int,
    val tiltYDegrees: Int,
    val state: Int,
    val buttons: Int,
    val tool: Int
)
{
    fun withState(value: Int): LtlPointerSample
    {
        return LtlPointerSample(pointerId, x, y, pressure, tiltXDegrees, tiltYDegrees, value, buttons, tool)
    }
}

@SuppressLint("ClickableViewAccessibility")
class LtlCaptureView(context: android.content.Context, private val inputSender: InputSender) : View(context)
{
    override fun onTouchEvent(event: MotionEvent): Boolean
    {
        requestUnbufferedDispatch(event)
        inputSender.publishTouchEvent(this, event)
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean
    {
        if (event.isFromSource(android.view.InputDevice.SOURCE_CLASS_POINTER))
        {
            requestUnbufferedDispatch(event)
            inputSender.publishHoverEvent(this, event)
            return true
        }
        return super.onGenericMotionEvent(event)
    }
}

class InputSender(
    private val host: String = WireProtocol.Host,
    private val inputPort: Int = WireProtocol.InputTcpPort
)
{
    companion object
    {
        const val OUTBOUND_QUEUE_SIZE = 384
        const val CONNECT_TIMEOUT_MS = 4000
        const val RECONNECT_BACKOFF_MS = 800
        const val QUEUE_POLL_TIMEOUT_MS = 120
    }

    private val running = AtomicBoolean(false)
    private val outbound = LinkedBlockingQueue<ByteArray>(OUTBOUND_QUEUE_SIZE)
    private var sender: Thread? = null

    fun start()
    {
        if (!running.compareAndSet(false, true))
        {
            return
        }
        val thread = Thread(object : Runnable
        {
            override fun run()
            {
                runSendLoop()
            }
        }, "ltl-input")
        thread.isDaemon = true
        sender = thread
        thread.start()
    }

    fun stop()
    {
        running.set(false)
        val thread = sender
        sender = null
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
    }

    fun requestKeyframe()
    {
        outbound.offer(assembleHeader(WireProtocol.PacketIdrRequest, 0))
    }

    fun closeSession()
    {
        outbound.offer(assembleHeader(WireProtocol.PacketBye, 0))
    }

    fun publishTouchEvent(view: View, event: MotionEvent)
    {
        if (running.get())
        {
            outbound.offer(encodeMotionEvent(WireProtocol.DeviceTouch, view, event))
        }
    }

    fun publishHoverEvent(view: View, event: MotionEvent)
    {
        if (running.get())
        {
            outbound.offer(encodeMotionEvent(WireProtocol.DevicePen, view, event))
        }
    }

    private fun runSendLoop()
    {
        while (running.get())
        {
            val socket = Socket()
            var connected = false
            try
            {
                socket.connect(InetSocketAddress(host, inputPort), CONNECT_TIMEOUT_MS)
                socket.tcpNoDelay = true
                connected = true
            }
            catch (ignored: IOException)
            {
            }

            if (connected)
            {
                try
                {
                    val output = socket.getOutputStream()
                    while (running.get())
                    {
                        val packet = outbound.poll(QUEUE_POLL_TIMEOUT_MS.toLong(), java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue
                        output.write(packet)
                        output.flush()
                    }
                }
                catch (ignored: IOException)
                {
                }
                finally
                {
                    try
                    {
                        socket.close()
                    }
                    catch (ignored: IOException)
                    {
                    }
                }
            }
            else
            {
                try
                {
                    socket.close()
                }
                catch (ignored: IOException)
                {
                }
            }

            if (!running.get())
            {
                break
            }
            try
            {
                Thread.sleep(RECONNECT_BACKOFF_MS.toLong())
            }
            catch (ignored: InterruptedException)
            {
                break
            }
        }
    }

    private fun assembleHeader(packetType: Int, payloadSize: Int): ByteArray
    {
        val header = ByteArray(WireProtocol.PacketHeaderSize)
        header[0] = WireProtocol.Magic0.toByte()
        header[1] = WireProtocol.Magic1.toByte()
        header[2] = packetType.toByte()
        header[3] = WireProtocol.ProtocolVersion.toByte()
        header[4] = (payloadSize and 0xFF).toByte()
        header[5] = ((payloadSize shr 8) and 0xFF).toByte()
        header[6] = ((payloadSize shr 16) and 0xFF).toByte()
        header[7] = ((payloadSize shr 24) and 0xFF).toByte()
        return header
    }

    private fun assemblePacket(packetType: Int, payload: ByteArray): ByteArray
    {
        val packet = ByteArray(WireProtocol.PacketHeaderSize + payload.size)
        System.arraycopy(assembleHeader(packetType, payload.size), 0, packet, 0, WireProtocol.PacketHeaderSize)
        System.arraycopy(payload, 0, packet, WireProtocol.PacketHeaderSize, payload.size)
        return packet
    }

    private fun encodeMotionEvent(deviceKind: Int, view: View, event: MotionEvent): ByteArray
    {
        val samples = ArrayList<LtlPointerSample>(WireProtocol.MaxInputPointers)
        val pointerLimit = event.pointerCount.coerceAtMost(WireProtocol.MaxInputPointers)
        val startingState = initialPointerState(deviceKind, event)

        for (index in 0 until pointerLimit)
        {
            val orientation = event.getAxisValue(MotionEvent.AXIS_ORIENTATION, index)
            val tilt = event.getAxisValue(MotionEvent.AXIS_TILT, index)
            val tiltDegrees = Math.toDegrees(tilt.toDouble())
            val tiltX = (tiltDegrees * Math.cos(orientation.toDouble())).toInt()
            val tiltY = (tiltDegrees * Math.sin(orientation.toDouble())).toInt()

            samples.add(LtlPointerSample(
                pointerId = event.getPointerId(index),
                x = normalize(event.getX(index) / view.width),
                y = normalize(event.getY(index) / view.height),
                pressure = normalize(event.getPressure(index)),
                tiltXDegrees = tiltX,
                tiltYDegrees = tiltY,
                state = startingState,
                buttons = stylusButtons(event.buttonState),
                tool = toolKind(event.getToolType(index))
            ))
        }

        when (event.actionMasked)
        {
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP ->
            {
                val liftedPointerId = event.getPointerId(event.actionIndex)
                for (index in samples.indices)
                {
                    val sample = samples[index]
                    samples[index] = sample.withState(
                        if (sample.pointerId == liftedPointerId) WireProtocol.PointerStateIdle else sample.state
                    )
                }
            }

            MotionEvent.ACTION_CANCEL ->
            {
                for (index in samples.indices)
                {
                    samples[index] = samples[index].withState(WireProtocol.PointerStateIdle)
                }
            }

            else ->
            {
            }
        }

        return encodeEvent(deviceKind, event.actionMasked, event.actionIndex, samples)
    }

    private fun initialPointerState(deviceKind: Int, event: MotionEvent): Int
    {
        if (deviceKind == WireProtocol.DevicePen)
        {
            if (event.actionMasked == MotionEvent.ACTION_HOVER_EXIT)
            {
                return WireProtocol.PointerStateIdle
            }
            if (event.actionMasked == MotionEvent.ACTION_HOVER_ENTER || event.actionMasked == MotionEvent.ACTION_HOVER_MOVE)
            {
                return WireProtocol.PointerStateHovering
            }
        }
        return WireProtocol.PointerStateContact
    }

    private fun stylusButtons(buttonState: Int): Int
    {
        var buttons = WireProtocol.ButtonNone
        if (buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0)
        {
            buttons = buttons or WireProtocol.ButtonStylusPrimary
        }
        if (buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY != 0)
        {
            buttons = buttons or WireProtocol.ButtonStylusSecondary
        }
        return buttons
    }

    private fun toolKind(androidTool: Int): Int
    {
        return when (androidTool)
        {
            MotionEvent.TOOL_TYPE_STYLUS -> WireProtocol.ToolPen
            MotionEvent.TOOL_TYPE_ERASER -> WireProtocol.ToolEraser
            MotionEvent.TOOL_TYPE_FINGER -> WireProtocol.ToolFinger
            MotionEvent.TOOL_TYPE_MOUSE -> WireProtocol.ToolMouse
            else -> WireProtocol.ToolUnknown
        }
    }

    private fun normalize(value: Float): Float
    {
        return when
        {
            value.isNaN() -> 0f
            value < 0f -> 0f
            value > 1f -> 1f
            else -> value
        }
    }

    private fun encodeEvent(deviceKind: Int, action: Int, actionIndex: Int, samples: List<LtlPointerSample>): ByteArray
    {
        val payload = java.nio.ByteBuffer.allocate(WireProtocol.InputEventPayloadSize)
        payload.order(java.nio.ByteOrder.LITTLE_ENDIAN)
        payload.put(deviceKind.toByte())
        payload.put(action.toByte())
        payload.put(samples.size.coerceAtMost(WireProtocol.MaxInputPointers).toByte())
        payload.put(actionIndex.toByte())

        var index = 0
        while (index < WireProtocol.MaxInputPointers)
        {
            if (index < samples.size)
            {
                val sample = samples[index]
                payload.putFloat(sample.x)
                payload.putFloat(sample.y)
                payload.putFloat(sample.pressure)
                payload.putShort(sample.tiltXDegrees.toShort())
                payload.putShort(sample.tiltYDegrees.toShort())
                payload.put(sample.pointerId.toByte())
                payload.put(sample.state.toByte())
                payload.put(sample.buttons.toByte())
                payload.put(sample.tool.toByte())
            }
            else
            {
                payload.position(payload.position() + WireProtocol.PointerPayloadSize)
            }
            index += 1
        }
        return assemblePacket(WireProtocol.PacketInput, payload.array())
    }
}
