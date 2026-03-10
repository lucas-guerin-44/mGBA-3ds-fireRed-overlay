package com.mgba.companion.data

import android.content.Context
import android.content.SharedPreferences
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import kotlin.random.Random

/**
 * SharedPreferences-backed storage for up to SLOT_COUNT walker mons.
 * Each slot is independent; all keys are prefixed with "sN_".
 */
class WalkerStore(context: Context) {

    companion object {
        const val SLOT_COUNT = 3
    }

    private val prefs: SharedPreferences =
        context.getSharedPreferences("walker_data", Context.MODE_PRIVATE)

    data class WalkerMon(
        val slot: Int,
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

    private fun key(slot: Int, name: String) = "s${slot}_$name"

    // ── Slot queries ──────────────────────────────────────────────────────────

    fun hasMonInSlot(slot: Int): Boolean = prefs.contains(key(slot, "raw_blob"))

    fun hasAnyActiveMon(): Boolean = (0 until SLOT_COUNT).any { hasMonInSlot(it) }

    fun getMonInSlot(slot: Int): WalkerMon? {
        val blobStr = prefs.getString(key(slot, "raw_blob"), null) ?: return null
        val blob = Base64.decode(blobStr, Base64.NO_WRAP)
        return WalkerMon(
            slot = slot,
            rawBlob = blob,
            species = prefs.getInt(key(slot, "species"), 0),
            nickname = prefs.getString(key(slot, "nickname"), "") ?: "",
            level = prefs.getInt(key(slot, "level"), 0),
            sentAt = prefs.getLong(key(slot, "sent_at"), 0),
            stepBaseline = prefs.getInt(key(slot, "step_baseline"), 0),
            totalSteps = prefs.getInt(key(slot, "total_steps"), 0),
            bonusXp = prefs.getInt(key(slot, "bonus_xp"), 0),
            foundItems = loadItems(slot),
            routeKey = prefs.getString(key(slot, "route_key"), null) ?: Routes.DEFAULT.key
        )
    }

    // ── Slot mutations ────────────────────────────────────────────────────────

    fun storeMonInSlot(
        slot: Int,
        blob: ByteArray,
        info: Gen3Decoder.MonInfo,
        stepBaseline: Int,
        routeKey: String = Routes.DEFAULT.key
    ) {
        prefs.edit()
            .putString(key(slot, "raw_blob"), Base64.encodeToString(blob, Base64.NO_WRAP))
            .putInt(key(slot, "species"), info.species)
            .putString(key(slot, "nickname"), info.nickname)
            .putInt(key(slot, "level"), info.level)
            .putLong(key(slot, "sent_at"), System.currentTimeMillis())
            .putInt(key(slot, "step_baseline"), stepBaseline)
            .putInt(key(slot, "total_steps"), 0)
            .putInt(key(slot, "bonus_xp"), 0)
            .putString(key(slot, "found_items"), "[]")
            .putInt(key(slot, "last_item_roll_steps"), 0)
            .putString(key(slot, "route_key"), routeKey)
            .apply()
    }

    fun setRouteForSlot(slot: Int, routeKey: String) {
        prefs.edit().putString(key(slot, "route_key"), routeKey).apply()
    }

    fun updateStepsForSlot(slot: Int, currentSensorValue: Int) {
        val baseline = prefs.getInt(key(slot, "step_baseline"), currentSensorValue)
        val newTotal = (currentSensorValue - baseline).coerceAtLeast(0)
        prefs.edit()
            .putInt(key(slot, "total_steps"), newTotal)
            .putInt(key(slot, "bonus_xp"), calculateXp(slot, newTotal))
            .apply()
        rollItems(slot, newTotal)
    }

    fun recalculateXpForSlot(slot: Int) {
        val steps = prefs.getInt(key(slot, "total_steps"), 0)
        prefs.edit().putInt(key(slot, "bonus_xp"), calculateXp(slot, steps)).apply()
    }

    fun recalculateAllXp() {
        for (slot in 0 until SLOT_COUNT) {
            if (hasMonInSlot(slot)) recalculateXpForSlot(slot)
        }
    }

    fun buildReturnPayloadForSlot(slot: Int): String? {
        val mon = getMonInSlot(slot) ?: return null
        val blobB64 = Base64.encodeToString(mon.rawBlob, Base64.NO_WRAP)
        val itemStr = mon.foundItems.joinToString(",") { "${it.itemId}x${it.qty}" }
        return "PK2:RECV:$blobB64:${mon.bonusXp}:$itemStr"
    }

    fun clearMonInSlot(slot: Int) {
        prefs.edit()
            .remove(key(slot, "raw_blob"))
            .remove(key(slot, "species"))
            .remove(key(slot, "nickname"))
            .remove(key(slot, "level"))
            .remove(key(slot, "sent_at"))
            .remove(key(slot, "step_baseline"))
            .remove(key(slot, "total_steps"))
            .remove(key(slot, "bonus_xp"))
            .remove(key(slot, "found_items"))
            .remove(key(slot, "last_item_roll_steps"))
            .remove(key(slot, "route_key"))
            .apply()
    }

    // ── Backward-compat helpers used by PokemonWidgetProvider / StepWorker ───

    /** True if any slot has an active mon. */
    fun hasActiveMon(): Boolean = hasAnyActiveMon()

    /** Returns the first occupied slot's mon (for widget display). */
    fun getActiveMon(): WalkerMon? =
        (0 until SLOT_COUNT).firstNotNullOfOrNull { getMonInSlot(it) }

    /** Recalculate passive XP for all occupied slots. */
    fun recalculateXp() = recalculateAllXp()

    // ── Private helpers ───────────────────────────────────────────────────────

    private fun getRouteForSlot(slot: Int): Routes.Route =
        Routes.get(prefs.getString(key(slot, "route_key"), null))

    private fun calculateXp(slot: Int, totalSteps: Int): Int {
        val level = prefs.getInt(key(slot, "level"), 1).coerceAtLeast(1)
        val sentAt = prefs.getLong(key(slot, "sent_at"), System.currentTimeMillis())
        val scale = (level / 10).coerceIn(1, 10)

        val stepXp = (totalSteps / 2) * scale

        val elapsedMin = ((System.currentTimeMillis() - sentAt) / 60_000).toInt()
            .coerceAtMost(24 * 60)
        val passiveXp = passiveXpForMinutes(elapsedMin) * scale

        return stepXp + passiveXp
    }

    private fun passiveXpForMinutes(minutes: Int): Int {
        var xp = 0
        val t1 = minutes.coerceAtMost(240)
        xp += t1 / 10
        val t2 = (minutes - 240).coerceIn(0, 240)
        xp += t2 / 20
        val t3 = (minutes - 480).coerceIn(0, 480)
        xp += t3 / 40
        val t4 = (minutes - 960).coerceIn(0, 480)
        xp += t4 / 80
        return xp
    }

    private fun rollItems(slot: Int, totalSteps: Int) {
        val lastRoll = prefs.getInt(key(slot, "last_item_roll_steps"), 0)
        val items = loadItems(slot).toMutableList()
        if (items.size >= 3) return

        val route = getRouteForSlot(slot)
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
            .putString(key(slot, "found_items"), arr.toString())
            .putInt(
                key(slot, "last_item_roll_steps"),
                (totalSteps / route.lootStepInterval) * route.lootStepInterval
            )
            .apply()
    }

    private fun loadItems(slot: Int): List<ItemDrop> {
        val json = prefs.getString(key(slot, "found_items"), "[]") ?: "[]"
        val arr = JSONArray(json)
        return (0 until arr.length()).map { i ->
            val obj = arr.getJSONObject(i)
            ItemDrop(obj.getInt("id"), obj.getString("name"), obj.getInt("qty"))
        }
    }
}