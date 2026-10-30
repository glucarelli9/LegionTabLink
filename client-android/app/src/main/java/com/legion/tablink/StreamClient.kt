package com.legion.tablink

import android.util.Log
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

object WireProtocol
{
    const val Host = "127.0.0.1"
    const val VideoTcpPort = 27183
    const val InputTcpPort = 27184
    const val ProtocolVersion = 1
    const val Magic0 = 0x4C
    const val Magic1 = 0x54
    const val PacketHeaderSize = 8
    const val MaxPayloadSize = 16 * 1024 * 1024

    const val PacketHello = 0x01
    const val PacketConfig = 0x02
    const val PacketVideo = 0x03
    const val PacketIdrRequest = 0x04
    const val PacketInput = 0x05
    const val PacketBye = 0x06

    const val VideoFrameHeaderSize = 4
    const val ConfigPayloadSize = 12
    const val HelloPayloadSize = 8

    const val CodecHevc = 0x01
    const val CodecAvc = 0x02

    const val InputEventPayloadHeader = 4
    const val PointerPayloadSize = 20
    const val MaxInputPointers = 10
    const val InputEventPayloadSize = InputEventPayloadHeader + MaxInputPointers * PointerPayloadSize

    const val DevicePen = 0x01
    const val DeviceTouch = 0x02

    const val PointerStateIdle = 0x00
    const val PointerStateHovering = 0x01
    const val PointerStateContact = 0x02

    const val ButtonNone = 0x00
    const val ButtonStylusPrimary = 0x01
    const val ButtonStylusSecondary = 0x02

    const val ToolUnknown = 0x00
    const val ToolPen = 0x01
    const val ToolEraser = 0x02
    const val ToolFinger = 0x03
    const val ToolMouse = 0x04
}

interface ConnectionListener
{
    fun onConnecting()
    fun onConnected(width: Int, height: Int, fps: Int)
    fun onDisconnected()
}

class StreamClient(
    private val decoder: LowLatencyDecoder,
    private val tabletMaxWidth: Int,
    private val tabletMaxHeight: Int,
    private val host: String = WireProtocol.Host,
    private val videoPort: Int = WireProtocol.VideoTcpPort
)
{
    companion object
    {
        const val RECONNECT_BACKOFF_MS = 1000
        const val READ_BUFFER_SIZE = 128 * 1024
    }

    var connectionListener: ConnectionListener? = null
    private val running = AtomicBoolean(false)
    private var reader: Thread? = null

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
                runConnectLoop()
            }
        }, "ltl-video")
        thread.isDaemon = true
        reader = thread
        thread.start()
    }

    fun stop()
    {
        running.set(false)
        val thread = reader
        reader = null
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

    private fun runConnectLoop()
    {
        while (running.get())
        {
            connectionListener?.onConnecting()
            val socket = Socket()
            try
            {
                Log.d("LTL", "Connecting to $host:$videoPort...")
                socket.connect(InetSocketAddress(host, videoPort), 4000)
                socket.tcpNoDelay = true
                Log.d("LTL", "Connected, sending hello")
                sendHello(socket)
                Log.d("LTL", "Hello sent, receiving packets")
                receivePackets(socket)
                Log.d("LTL", "receivePackets returned")
            }
            catch (e: IOException)
            {
                Log.e("LTL", "Connection error", e)
            }
            finally
            {
                connectionListener?.onDisconnected()
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

    private fun sendHello(socket: Socket)
    {
        val payload = ByteArray(WireProtocol.HelloPayloadSize)
        val buffer = java.nio.ByteBuffer.wrap(payload)
        buffer.order(java.nio.ByteOrder.LITTLE_ENDIAN)
        buffer.putShort(tabletMaxWidth.coerceAtMost(Short.MAX_VALUE.toInt()).toShort())
        buffer.putShort(tabletMaxHeight.coerceAtMost(Short.MAX_VALUE.toInt()).toShort())
        buffer.put(WireProtocol.CodecHevc.toByte())
        buffer.put(0)
        buffer.put(0)
        buffer.put(0)
        writePacket(socket, WireProtocol.PacketHello, payload)
    }

    private fun receivePackets(socket: Socket)
    {
        val source = DataInputStream(BufferedInputStream(socket.getInputStream(), READ_BUFFER_SIZE))
        val header = ByteArray(WireProtocol.PacketHeaderSize)
        while (running.get())
        {
            val headerData = readFully(source, header.size)
            if (headerData == null) {
                Log.e("LTL", "receivePackets: header read returned null, exiting")
                return
            }
            System.arraycopy(headerData, 0, header, 0, header.size)
            val magic0 = header[0].toInt() and 0xFF
            val magic1 = header[1].toInt() and 0xFF
            Log.d("LTL", "Packet: magic=0x${String.format("%02X%02X", magic0, magic1)} type=${header[2].toInt() and 0xFF} ver=${header[3].toInt() and 0xFF}")
            if (magic0 != WireProtocol.Magic0 || magic1 != WireProtocol.Magic1
                || (header[3].toInt() and 0xFF) != WireProtocol.ProtocolVersion)
            {
                Log.e("LTL", "Invalid header magic/version, exiting")
                return
            }
            val payloadSize = ((header[4].toInt() and 0xFF)
                or ((header[5].toInt() and 0xFF) shl 8)
                or ((header[6].toInt() and 0xFF) shl 16)
                or ((header[7].toInt() and 0xFF) shl 24))
            if (payloadSize > WireProtocol.MaxPayloadSize)
            {
                return
            }

            when (header[2].toInt() and 0xFF)
            {
                WireProtocol.PacketConfig ->
                {
                    if (payloadSize != WireProtocol.ConfigPayloadSize)
                    {
                        return
                    }
                    val payload = readFully(source, payloadSize) ?: return
                    val parameters = java.nio.ByteBuffer.wrap(payload)
                    parameters.order(java.nio.ByteOrder.LITTLE_ENDIAN)
                    val width = parameters.short.toInt() and 0xFFFF
                    val height = parameters.short.toInt() and 0xFFFF
                    val fps = parameters.short.toInt() and 0xFFFF
                    val bitrateMbps = parameters.short.toInt() and 0xFFFF
                    val codec = parameters.get().toInt() and 0xFF
                    Log.d("LTL", "Config: ${width}x${height} @ $fps fps, codec=$codec")
                    decoder.onStreamConfigured(width, height, codec)
                    connectionListener?.onConnected(width, height, fps)
                }

                WireProtocol.PacketVideo ->
                {
                    if (payloadSize < WireProtocol.VideoFrameHeaderSize)
                    {
                        Log.w("LTL", "Video packet too small: $payloadSize")
                        return
                    }
                    val frameHeader = readFully(source, WireProtocol.VideoFrameHeaderSize) ?: return
                    val keyframe = frameHeader[0].toInt() != 0
                    val frameSize = payloadSize - WireProtocol.VideoFrameHeaderSize
                    val frame = readFully(source, frameSize) ?: return
                    decoder.pushEncodedFrame(frame, keyframe)
                }

                WireProtocol.PacketIdrRequest ->
                {
                    drainPayload(source, payloadSize)
                }

                WireProtocol.PacketBye ->
                {
                    drainPayload(source, payloadSize)
                    return
                }

                else ->
                {
                    drainPayload(source, payloadSize)
                }
            }
        }
    }

    private fun writePacket(socket: Socket, packetType: Int, payload: ByteArray)
    {
        val total = WireProtocol.PacketHeaderSize + payload.size
        val packet = ByteArray(total)
        val buffer = java.nio.ByteBuffer.wrap(packet)
        buffer.order(java.nio.ByteOrder.LITTLE_ENDIAN)
        buffer.put(WireProtocol.Magic0.toByte())
        buffer.put(WireProtocol.Magic1.toByte())
        buffer.put(packetType.toByte())
        buffer.put(WireProtocol.ProtocolVersion.toByte())
        buffer.putInt(payload.size)
        buffer.put(payload)
        val output = socket.getOutputStream()
        output.write(packet)
        output.flush()
    }

    private fun readFully(source: DataInputStream, size: Int): ByteArray?
    {
        if (size == 0)
        {
            return ByteArray(0)
        }
        val data = ByteArray(size)
        try
        {
            source.readFully(data)
        }
        catch (exception: IOException)
        {
            Log.e("LTL", "readFully($size) failed: ${exception.message}")
            return null
        }
        return data
    }

    private fun drainPayload(source: DataInputStream, size: Int)
    {
        if (size <= 0)
        {
            return
        }
        readFully(source, size)
    }
}
