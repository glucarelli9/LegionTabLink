package com.legion.tablink

import android.animation.ArgbEvaluator
import android.animation.TimeAnimator
import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.util.AttributeSet
import android.view.View
import kotlin.math.min
import kotlin.math.sin

object Palette
{
    const val Bg = 0xFF09090F.toInt()
    const val Accent = 0xFF6D6AF7.toInt()
    const val Accent2 = 0xFFA855F7.toInt()
    const val Ok = 0xFF34D399.toInt()
    const val Warn = 0xFFFBBF24.toInt()

    fun withAlpha(color: Int, alpha: Float): Int =
        Color.argb((alpha * 255).toInt().coerceIn(0, 255), Color.red(color), Color.green(color), Color.blue(color))
}

/** Base class: runs a frame clock only while the view is actually on screen. */
abstract class AnimatedView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null)
    : View(context, attrs)
{
    protected var timeSec = 0f
    private val clock = TimeAnimator().apply {
        setTimeListener { _, total, _ -> timeSec = total / 1000f; invalidate() }
    }
    private var stateAnim: ValueAnimator? = null
    protected var stateColor = Palette.Warn
    protected var connectedness = 0f

    fun setConnected(connected: Boolean)
    {
        val targetColor = if (connected) Palette.Ok else Palette.Warn
        val targetMix = if (connected) 1f else 0f
        val fromColor = stateColor
        val fromMix = connectedness
        stateAnim?.cancel()
        stateAnim = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 700
            addUpdateListener {
                val f = it.animatedFraction
                stateColor = ArgbEvaluator().evaluate(f, fromColor, targetColor) as Int
                connectedness = fromMix + (targetMix - fromMix) * f
                invalidate()
            }
            start()
        }
    }

    override fun onVisibilityAggregated(isVisible: Boolean)
    {
        super.onVisibilityAggregated(isVisible)
        if (isVisible && !clock.isStarted) clock.start() else if (!isVisible) clock.end()
    }

    override fun onDetachedFromWindow()
    {
        clock.end()
        stateAnim?.cancel()
        super.onDetachedFromWindow()
    }
}

class GlowBackgroundView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null)
    : AnimatedView(context, attrs)
{
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun onDraw(canvas: Canvas)
    {
        canvas.drawColor(Palette.Bg)
        val w = width.toFloat()
        val h = height.toFloat()
        val r = maxOf(w, h) * 0.55f
        val drift = sin(timeSec * 0.25f) * 0.04f

        glow(canvas, w * (0.08f + drift), h * (0.02f - drift), r, Palette.Accent, 0.30f)
        val second = ArgbEvaluator().evaluate(connectedness, Palette.Accent2, Palette.Ok) as Int
        glow(canvas, w * (0.95f - drift), h * (1.0f + drift), r * 0.9f, second, 0.20f)
    }

    private fun glow(canvas: Canvas, cx: Float, cy: Float, radius: Float, color: Int, alpha: Float)
    {
        paint.shader = RadialGradient(cx, cy, radius,
            Palette.withAlpha(color, alpha), Color.TRANSPARENT, Shader.TileMode.CLAMP)
        canvas.drawCircle(cx, cy, radius, paint)
    }
}

/** Laptop → tablet illustration, drawn in a 300×120 design space. */
class LinkSceneView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null)
    : AnimatedView(context, attrs)
{
    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 1.6f; color = Palette.withAlpha(Color.WHITE, 0.22f)
    }
    private val bodyFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Palette.withAlpha(Color.WHITE, 0.03f) }
    private val screenFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val line = Paint(Paint.ANTI_ALIAS_FLAG)
    private val link = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2f; strokeCap = Paint.Cap.ROUND
    }
    private val packet = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()
    private val base = Path().apply { moveTo(8f, 96f); lineTo(150f, 96f); lineTo(142f, 104f); lineTo(16f, 104f); close() }
    private val screenShader = LinearGradient(0f, 0f, 300f, 120f,
        Palette.withAlpha(Palette.Accent, 0.6f), Palette.withAlpha(Palette.Accent2, 0.4f), Shader.TileMode.CLAMP)

    override fun onDraw(canvas: Canvas)
    {
        val scale = min(width / 300f, height / 120f)
        canvas.save()
        canvas.translate((width - 300f * scale) / 2f, (height - 120f * scale) / 2f)
        canvas.scale(scale, scale)

        // laptop
        roundRect(canvas, 20f, 14f, 118f, 76f, 7f, bodyFill, outline)
        screenFill.shader = screenShader; screenFill.alpha = 255
        rect.set(27f, 21f, 131f, 83f); canvas.drawRoundRect(rect, 3f, 3f, screenFill)
        contentLines(canvas, 38f, 31f, floatArrayOf(40f, 70f, 56f), 9f)
        canvas.drawPath(base, bodyFill); canvas.drawPath(base, outline)

        // link
        val flowSpeed = 12f + 20f * connectedness
        link.color = stateColor
        link.alpha = (140 + 100 * connectedness).toInt()
        link.pathEffect = DashPathEffect(floatArrayOf(3f, 7f), -timeSec * flowSpeed)
        canvas.drawLine(146f, 58f, 226f, 58f, link)
        if (connectedness > 0.01f)
        {
            packet.color = stateColor
            for (i in 0 until 3)
            {
                val t = ((timeSec / 1.4f) + i / 3f) % 1f
                val fade = (if (t < 0.15f) t / 0.15f else if (t > 0.85f) (1f - t) / 0.15f else 1f) * connectedness
                packet.alpha = (255 * fade).toInt()
                canvas.drawCircle(146f + 80f * t, 58f, 3f, packet)
            }
        }

        // tablet
        roundRect(canvas, 232f, 20f, 60f, 86f, 8f, bodyFill, outline)
        screenFill.shader = null; screenFill.color = Palette.withAlpha(Color.WHITE, 0.04f)
        rect.set(238f, 27f, 286f, 99f); canvas.drawRoundRect(rect, 3f, 3f, screenFill)
        if (connectedness > 0.01f)
        {
            screenFill.shader = screenShader; screenFill.alpha = (255 * connectedness).toInt()
            canvas.drawRoundRect(rect, 3f, 3f, screenFill)
            contentLines(canvas, 244f, 35f, floatArrayOf(22f, 34f, 28f), 7.5f, connectedness)
        }
        canvas.restore()
    }

    private fun roundRect(canvas: Canvas, x: Float, y: Float, w: Float, h: Float, r: Float, fill: Paint, stroke: Paint)
    {
        rect.set(x, y, x + w, y + h)
        canvas.drawRoundRect(rect, r, r, fill)
        canvas.drawRoundRect(rect, r, r, stroke)
    }

    private fun contentLines(canvas: Canvas, x: Float, y: Float, widths: FloatArray, gap: Float, alpha: Float = 1f)
    {
        widths.forEachIndexed { i, w ->
            line.color = Palette.withAlpha(Color.WHITE, (if (i == 0) 0.35f else 0.18f) * alpha)
            val h = if (i == 0) 5f else 4f
            rect.set(x, y + i * gap, x + w, y + i * gap + h)
            canvas.drawRoundRect(rect, h / 2f, h / 2f, line)
        }
    }
}
