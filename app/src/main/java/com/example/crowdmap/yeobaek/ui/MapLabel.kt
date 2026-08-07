package com.example.crowdmap.yeobaek.ui

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.util.DisplayMetrics
import kotlin.math.max

/**
 * 지도 위 장소 라벨 — '꼬리 달린 알약(pill) 칩'.
 *
 * 글자만 띄우면 구글 지도가 원래 그리는 지명(안국동·재동…)과 섞여 무엇이 우리 장소인지
 * 구분되지 않는다. 그래서 **채운 배경 + 흰 글씨 + 흰 테두리**로 칩을 그려 지도 위에서
 * 확실히 튀게 하고, 칩 아래 작은 꼬리가 실제 좌표를 가리키게 한다.
 * 칩 색 자체가 혼잡 레벨을 나타내므로 색과 이름을 한 번에 읽을 수 있다.
 */
object MapLabel {

    /** 칩이 길어지면 지도를 덮으므로 줄여서 표시. */
    private const val MAX_CHARS = 8

    fun shorten(title: String): String =
        if (title.length <= MAX_CHARS) title else title.take(MAX_CHARS - 1) + "…"

    /** 그려진 라벨 + 지도 좌표에 맞출 앵커(꼬리 끝). */
    data class Label(val bitmap: Bitmap, val anchorX: Float, val anchorY: Float)

    /**
     * @param fillColor 칩 배경색(혼잡 레벨 색 또는 브랜드 색)
     * @param emphasize 담은 장소처럼 강조할 때 true — 글자·칩을 조금 키운다
     */
    fun render(
        dm: DisplayMetrics,
        title: String,
        fillColor: Int,
        emphasize: Boolean = false,
    ): Label {
        val d = dm.density
        val text = shorten(title)

        val padH = 9f * d
        val padV = 5f * d
        val borderW = 2f * d          // 흰 테두리 — 칩끼리, 배경과 분리
        val tailH = 6f * d
        val tailW = 9f * d
        val shadowPad = 1.5f * d

        val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = (if (emphasize) 12.5f else 11.5f) * d
            typeface = Typeface.create(Typeface.DEFAULT_BOLD, Typeface.BOLD)
            textAlign = Paint.Align.CENTER
            color = Color.WHITE
        }
        val fm = textPaint.fontMetrics
        val textH = fm.descent - fm.ascent
        val textW = textPaint.measureText(text)

        val pillW = textW + padH * 2
        val pillH = textH + padV * 2
        val w = pillW + borderW * 2 + shadowPad * 2
        val h = pillH + borderW * 2 + tailH + shadowPad * 2

        val bmp = Bitmap.createBitmap(
            max(1, Math.ceil(w.toDouble()).toInt()),
            max(1, Math.ceil(h.toDouble()).toInt()),
            Bitmap.Config.ARGB_8888,
        )
        val c = Canvas(bmp)
        val cx = bmp.width / 2f

        val left = (bmp.width - pillW) / 2f
        val top = borderW + shadowPad
        val pill = RectF(left, top, left + pillW, top + pillH)
        val r = pillH / 2f     // 알약 모양

        // 1) 옅은 그림자 — 밝은 지도 위에서 칩이 떠 보이게
        val shadow = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(38, 0, 0, 0) }
        c.drawRoundRect(
            RectF(pill.left, pill.top + shadowPad, pill.right, pill.bottom + shadowPad),
            r, r, shadow)

        // 2) 흰 테두리(꼬리까지 포함해 한 번에)
        val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = borderW * 2
            strokeJoin = Paint.Join.ROUND
        }
        val tailPath = Path().apply {
            moveTo(cx - tailW / 2f, pill.bottom - 1f)
            lineTo(cx + tailW / 2f, pill.bottom - 1f)
            lineTo(cx, pill.bottom + tailH)
            close()
        }
        c.drawRoundRect(pill, r, r, border)
        c.drawPath(tailPath, border)

        // 3) 본체 채우기
        val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = fillColor }
        c.drawRoundRect(pill, r, r, fill)
        c.drawPath(tailPath, fill)

        // 4) 이름
        c.drawText(text, cx, pill.top + padV - fm.ascent, textPaint)

        // 앵커는 꼬리 끝 — 실제 좌표를 정확히 가리킨다.
        return Label(bmp, 0.5f, (pill.bottom + tailH) / bmp.height)
    }
}
