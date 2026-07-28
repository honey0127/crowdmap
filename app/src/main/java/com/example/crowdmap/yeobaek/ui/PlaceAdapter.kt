package com.example.crowdmap.yeobaek.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.PlaceResult

/** 장소 검색 결과 어댑터. 항목 탭 → 홈에 추가. */
class PlaceAdapter(
    private val onPick: (PlaceResult) -> Unit,
) : RecyclerView.Adapter<PlaceAdapter.VH>() {

    private val items = mutableListOf<PlaceResult>()

    fun submit(list: List<PlaceResult>) {
        items.clear(); items.addAll(list); notifyDataSetChanged()
    }

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val title: TextView = v.findViewById(R.id.place_title)
        val meta: TextView = v.findViewById(R.id.place_meta)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_place_result, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(h: VH, position: Int) {
        val p = items[position]
        h.title.text = p.title
        val meta = listOfNotNull(p.catLabel, p.addr).joinToString(" · ")
        h.meta.text = meta
        h.meta.visibility = if (meta.isBlank()) View.GONE else View.VISIBLE
        h.itemView.setOnClickListener { onPick(p) }
    }

    override fun getItemCount(): Int = items.size
}
