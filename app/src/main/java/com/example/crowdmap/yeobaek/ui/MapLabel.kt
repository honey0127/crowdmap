package com.example.crowdmap.yeobaek.ui

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.util.DisplayMetrics
import kotlin.math.max

/**
 * 네이버지도식 장소 라벨.
 *
 * 큼직한 핀 대신 **작은 점 + 장소 이름**을 그린다. 핀만 잔뜩 찍히면 무엇이 무엇인지
 * 구분이 안 되므로, 이름을 직접 보여주고 점 색으로 혼잡도를 표현한다.
 * 글자는 흰 테두리(halo)를 둘러 어떤 지도 배경 위에서도 읽히게 한다.
 */
object MapLabel {

    /** 라벨이 너무 길어지면 지도를 덮으므로 줄여서 표시. */
    private const val MAX_CHARS = 9

    fun shorten(title: String): String =
        if (title.length <= MAX_CHARS) title else title.take(MAX_CHARS - 1) + "…"

    /** 그려진 라벨 + 지도 좌표에 맞출 앵커(점 중심). */
    data class Label(val bitmap: Bitmap, val anchorX: Float, val anchorY: Float)

    /**
     * @param dotColor  점 색(혼잡 레벨 색)
     * @param emphasize 담은 장소처럼 강조할 때 true — 글자를 진하게, 점을 크게
     */
    fun render(
        dm: DisplayMetrics,
        title: String,
        dotColor: Int,
        emphasize: Boolean = false,
    ): Label {
        val d = dm.density
        val text = shorten(title)

        val dotR = (if (emphasize) 6.5f else 5f) * d
        val gap = 3f * d
        val haloW = 3.5f * d
        val pad = 2f * d

        val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = (if (emphasize) 12.5f else 11.5f) * d
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            textAlign = Paint.Align.CENTER
            color = if (emphasize) Color.parseColor("#0F3E42") else Color.parseColor("#2B3138")
        }
        val haloPaint = Paint(textPaint).apply {
            style = Paint.Style.STROKE
            strokeWidth = haloW
            strokeJoin = Paint.Join.ROUND
            color = Color.WHITE
        }

        val fm = textPaint.fontMetrics
        val textH = fm.descent - fm.ascent
        val textW = textPaint.measureText(text)

        val w = max(textW + haloW * 2 + pad * 2, dotR * 2 + pad * 2)
        val h = dotR * 2 + gap + textH + pad
        val bmp = Bitmap.createBitmap(
            max(1, Math.ceil(w.toDouble()).toInt()),
            max(1, Math.ceil(h.toDouble()).toInt()),
            Bitmap.Config.ARGB_8888,
        )
        val c = Canvas(bmp)
        val cx = bmp.width / 2f

        // 점: 흰 테두리를 둘러 배경과 분리
        val dotCy = dotR
        c.drawCircle(cx, dotCy, dotR, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
        })
        c.drawCircle(cx, dotCy, dotR - 1.5f * d, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = dotColor
        })

        // 이름: 흰 halo → 본문 순서로 그려 가독성 확보
        val baseline = dotR * 2 + gap - fm.ascent
        c.drawText(text, cx, baseline, haloPaint)
        c.drawText(text, cx, baseline, textPaint)

        // 앵커는 '점 중심' — 실제 좌표에 점이 놓이고 이름은 그 아래에 걸린다.
        return Label(bmp, 0.5f, dotCy / bmp.height)
    }
}
