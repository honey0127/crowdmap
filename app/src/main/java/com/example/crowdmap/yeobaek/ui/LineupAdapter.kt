package com.example.crowdmap.yeobaek.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.Congestion
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.google.android.material.button.MaterialButton

/**
 * 지역구 명소 라인업. 각 항목 "＋ 담기 / 담김" 토글로 내 코스에 넣고 뺀다.
 * 선택 여부는 액티비티가 소유한 selectedIds 로 판단.
 */
class LineupAdapter(
    private val isSelected: (Long) -> Boolean,
    private val onToggle: (PlaceResult) -> Unit,
) : RecyclerView.Adapter<LineupAdapter.VH>() {

    private val items = mutableListOf<PlaceResult>()

    fun submit(list: List<PlaceResult>) {
        items.clear(); items.addAll(list); notifyDataSetChanged()
    }

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val index: TextView = v.findViewById(R.id.lineup_index)
        val title: TextView = v.findViewById(R.id.lineup_title)
        val meta: TextView = v.findViewById(R.id.lineup_meta)
        val toggle: MaterialButton = v.findViewById(R.id.lineup_toggle)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_lineup, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(h: VH, position: Int) {
        val p = items[position]
        h.index.text = (position + 1).toString()
        h.title.text = p.title
        val dist = p.distKm?.let { "%.1fkm".format(it) }
        val congestion = p.level?.let { Congestion.label(it) }
        h.meta.text = listOfNotNull(p.catLabel, congestion, dist).joinToString(" · ")
        val picked = isSelected(p.contentId)
        h.toggle.text = if (picked) "담김 ✓" else "＋ 담기"
        h.toggle.setOnClickListener { onToggle(p) }
    }

    override fun getItemCount(): Int = items.size
}
