package com.example.crowdmap.yeobaek.ui

import android.content.res.ColorStateList
import android.graphics.Color
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.District

/** 지역구 가로 칩. 선택된 칩은 그린 채움. */
class DistrictChipAdapter(
    private val onSelect: (District) -> Unit,
) : RecyclerView.Adapter<DistrictChipAdapter.VH>() {

    private val items = mutableListOf<District>()
    private var selectedKey: String? = null

    fun submit(list: List<District>, selected: String?) {
        items.clear(); items.addAll(list); selectedKey = selected; notifyDataSetChanged()
    }

    fun select(key: String) {
        if (selectedKey == key) return
        selectedKey = key; notifyDataSetChanged()
    }

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val chip: TextView = v.findViewById(R.id.district_chip)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_district_chip, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(h: VH, position: Int) {
        val d = items[position]
        val ctx = h.chip.context
        h.chip.text = d.name
        val selected = d.key == selectedKey
        val primary = ContextCompat.getColor(ctx, R.color.ye_primary)
        val surface = ContextCompat.getColor(ctx, R.color.ye_surface)
        h.chip.backgroundTintList =
            ColorStateList.valueOf(if (selected) primary else surface)
        h.chip.setTextColor(
            if (selected) Color.WHITE else ContextCompat.getColor(ctx, R.color.ye_on_surface))
        h.chip.setOnClickListener { onSelect(d) }
    }

    override fun getItemCount(): Int = items.size
}
