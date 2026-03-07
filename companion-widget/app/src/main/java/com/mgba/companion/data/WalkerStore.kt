package com.mgba.companion.data

import android.content.Context
import android.content.SharedPreferences
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import kotlin.random.Random

/**
 * SharedPreferences-backed storage for the active walker mon.
 * Only one mon can be walking at a time.
 */
class WalkerStore(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences("walker_data", Context.MODE_PRIVATE)

    data class WalkerMon(
        val rawBlob: ByteArray,
        val species: Int,
        val nickname: String,
        val level: Int,
        val sentAt: Long,
        val stepBaseline: Int,
        val totalSteps: Int,
        val bonusXp: Int,
        val foundItems: List<ItemDrop>,
        val routeKey: String
    )

    data class ItemDrop(val itemId: Int, val name: String, val qty: Int)

    private fun getRoute(): Routes.Route = Routes.get(prefs.getString("route_key", null))

    fun hasActiveMon(): Boolean = prefs.contains("raw_blob")

    fun getActiveMon(): WalkerMon? {
        val blobStr = prefs.getString("raw_blob", null) ?: return null
        val blob = Base64.decode(blobStr, Base64.NO_WRAP)
        return WalkerMon(
            rawBlob = blob,
            species = prefs.getInt("species", 0),
            nickname = prefs.getString("nickname", "") ?: "",
            level = prefs.getInt("level", 0),
            sentAt = prefs.getLong("sent_at", 0),
            stepBaseline = prefs.getInt("step_baseline", 0),
            totalSteps = prefs.getInt("total_steps", 0),
            bonusXp = prefs.getInt("bonus_xp", 0),
            foundItems = loadItems(),
            routeKey = prefs.getString("route_key", null) ?: Routes.DEFAULT.key
        )
    }

    fun storeMon(blob: ByteArray, info: Gen3Decoder.MonInfo, stepBaseline: Int, routeKey: String = Routes.DEFAULT.key) {
        prefs.edit()
            .putString("raw_blob", Base64.encodeToString(blob, Base64.NO_WRAP))
            .putInt("species", info.species)
            .putString("nickname", info.nickname)
            .putInt("level", info.level)
            .putLong("sent_at", System.currentTimeMillis())
            .putInt("step_baseline", stepBaseline)
            .putInt("total_steps", 0)
            .putInt("bonus_xp", 0)
            .putString("found_items", "[]")
            .putInt("last_item_roll_steps", 0)
            .putString("route_key", routeKey)
            .apply()
    }

    fun setRoute(routeKey: String) {
        prefs.edit().putString("route_key", routeKey).apply()
    }

    fun updateSteps(currentSensorValue: Int) {
        val baseline = prefs.getInt("step_baseline", currentSensorValue)
        val newTotal = (currentSensorValue - baseline).coerceAtLeast(0)

        prefs.edit()
            .putInt("total_steps", newTotal)
            .putInt("bonus_xp", calculateXp(newTotal))
            .apply()

        // Check for item drops
        rollItems(newTotal)
    }

    /**
     * Recalculate total XP from steps + passive time.
     * Called by StepWorker periodically and WalkerActivity on resume.
     */
    fun recalculateXp() {
        val steps = prefs.getInt("total_steps", 0)
        prefs.edit()
            .putInt("bonus_xp", calculateXp(steps))
            .apply()
    }

    /**
     * XP = step XP + passive (time-based) XP, both scaled by level.
     *
     * Level scale: level / 10, clamped [1..10]. Keeps bonus relevant
     * against higher-level XP curves.
     *
     * Step XP:    (steps / 2) * scale  — uncapped, rewards walking
     *
     * Passive XP: diminishing returns to prevent overnight AFK abuse.
     *   First 4 hours:  full rate (1 XP / 10 min)
     *   4-8 hours:       half rate
     *   8-16 hours:      quarter rate
     *   16+ hours:       trickle (1/8 rate), hard cap at 24h of accrual
     *
     * At base scale, max passive ≈ 24 + 12 + 12 + 6 = 54 XP per day.
     * At Lv.50 (scale=5): ~270 passive/day. Steps still dominate.
     */
    private fun calculateXp(totalSteps: Int): Int {
        val level = prefs.getInt("level", 1).coerceAtLeast(1)
        val sentAt = prefs.getLong("sent_at", System.currentTimeMillis())
        val scale = (level / 10).coerceIn(1, 10)

        val stepXp = (totalSteps / 2) * scale

        val elapsedMin = ((System.currentTimeMillis() - sentAt) / 60_000).toInt()
            .coerceAtMost(24 * 60) // hard cap at 24h
        val passiveXp = passiveXpForMinutes(elapsedMin) * scale

        return stepXp + passiveXp
    }

    /** Diminishing passive XP: full rate tapers in 4-hour tiers. */
    private fun passiveXpForMinutes(minutes: Int): Int {
        var xp = 0
        // Tier 1: 0-4h (0-240 min) — 1 XP per 10 min
        val t1 = minutes.coerceAtMost(240)
        xp += t1 / 10
        // Tier 2: 4-8h (240-480 min) — 1 XP per 20 min
        val t2 = (minutes - 240).coerceIn(0, 240)
        xp += t2 / 20
        // Tier 3: 8-16h (480-960 min) — 1 XP per 40 min
        val t3 = (minutes - 480).coerceIn(0, 480)
        xp += t3 / 40
        // Tier 4: 16-24h (960-1440 min) — 1 XP per 80 min
        val t4 = (minutes - 960).coerceIn(0, 480)
        xp += t4 / 80
        return xp
    }

    private fun rollItems(totalSteps: Int) {
        val lastRoll = prefs.getInt("last_item_roll_steps", 0)
        val items = loadItems().toMutableList()
        if (items.size >= 3) return  // Max 3 items per walk

        val route = getRoute()
        val totalWeight = route.loot.sumOf { it.weight }
        var rollAt = lastRoll + route.lootStepInterval
        while (totalSteps >= rollAt && items.size < 3) {
            val roll = Random.nextInt(totalWeight)
            var cumulative = 0
            for (entry in route.loot) {
                cumulative += entry.weight
                if (roll < cumulative) {
                    val existing = items.indexOfFirst { it.itemId == entry.itemId }
                    if (existing >= 0) {
                        items[existing] = items[existing].copy(qty = items[existing].qty + 1)
                    } else {
                        items.add(ItemDrop(entry.itemId, entry.name, 1))
                    }
                    break
                }
            }
            rollAt += route.lootStepInterval
        }

        val arr = JSONArray()
        for (item in items) {
            arr.put(JSONObject().apply {
                put("id", item.itemId)
                put("name", item.name)
                put("qty", item.qty)
            })
        }
        prefs.edit()
            .putString("found_items", arr.toString())
            .putInt("last_item_roll_steps", (totalSteps / route.lootStepInterval) * route.lootStepInterval)
            .apply()
    }

    private fun loadItems(): List<ItemDrop> {
        val json = prefs.getString("found_items", "[]") ?: "[]"
        val arr = JSONArray(json)
        return (0 until arr.length()).map { i ->
            val obj = arr.getJSONObject(i)
            ItemDrop(obj.getInt("id"), obj.getString("name"), obj.getInt("qty"))
        }
    }

    /**
     * Build the PK2:RECV payload for returning the mon to the 3DS.
     */
    fun buildReturnPayload(): String? {
        val mon = getActiveMon() ?: return null
        val blobB64 = Base64.encodeToString(mon.rawBlob, Base64.NO_WRAP)
        val itemStr = mon.foundItems.joinToString(",") { "${it.itemId}x${it.qty}" }
        return "PK2:RECV:$blobB64:${mon.bonusXp}:$itemStr"
    }

    fun clearMon() {
        prefs.edit().clear().apply()
    }
}
