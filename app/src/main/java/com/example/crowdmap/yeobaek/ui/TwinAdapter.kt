package com.example.crowdmap.yeobaek.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.Congestion
import com.example.crowdmap.yeobaek.data.Twin

/** 감성 쌍둥이(대안) 목록 어댑터. 항목 탭 → 설득 카드. */
class TwinAdapter(
    private val items: List<Twin>,
    private val onPick: (Twin) -> Unit,
) : RecyclerView.Adapter<TwinAdapter.VH>() {

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val title: TextView = v.findViewById(R.id.twin_title)
        val meta: TextView = v.findViewById(R.id.twin_meta)
        val badge: TextView = v.findViewById(R.id.twin_badge)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_twin, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(h: VH, position: Int) {
        val t = items[position]
        h.title.text = t.title
        val sim = (t.similarity * 100).toInt()
        h.meta.text = "유사도 ${sim}% · ${t.distKm}km"
        h.badge.text = Congestion.label(t.forecastLevel)
        h.badge.setBackgroundColor(Congestion.color(t.forecastLevel))
        h.itemView.setOnClickListener { onPick(t) }
    }

    override fun getItemCount(): Int = items.size
}
