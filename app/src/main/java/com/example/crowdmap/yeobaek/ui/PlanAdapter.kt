package com.example.crowdmap.yeobaek.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.Congestion
import com.example.crowdmap.yeobaek.data.PlanStop

/** 플래너 타임라인 어댑터. 고혼잡 stop 에만 "대안 보기" 노출. */
class PlanAdapter(
    private val items: List<PlanStop>,
    private val onAlt: (PlanStop) -> Unit,
) : RecyclerView.Adapter<PlanAdapter.VH>() {

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val arrival: TextView = v.findViewById(R.id.stop_arrival)
        val title: TextView = v.findViewById(R.id.stop_title)
        val badge: TextView = v.findViewById(R.id.stop_badge)
        val sub: TextView = v.findViewById(R.id.stop_sub)
        val altBtn: Button = v.findViewById(R.id.stop_alt)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_plan_stop, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(h: VH, position: Int) {
        val s = items[position]
        h.arrival.text = s.arrival
        h.title.text = s.title
        h.badge.text = Congestion.label(s.forecastLevel)
        h.badge.setBackgroundColor(Congestion.color(s.forecastLevel))

        if (s.substitutedFrom != null) {
            h.sub.visibility = View.VISIBLE
            h.sub.text = "대안 스왑됨"
        } else {
            h.sub.visibility = View.GONE
        }

        if (Congestion.isHigh(s.forecastLevel)) {
            h.altBtn.visibility = View.VISIBLE
            h.altBtn.setOnClickListener { onAlt(s) }
        } else {
            h.altBtn.visibility = View.GONE
            h.altBtn.setOnClickListener(null)
        }
    }

    override fun getItemCount(): Int = items.size
}
