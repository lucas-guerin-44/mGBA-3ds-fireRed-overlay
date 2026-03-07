package com.mgba.companion.data

import android.content.Context
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL

class PokemonRepository(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences("pokemon_cache", Context.MODE_PRIVATE)

    val ip: String
        get() = settingsPrefs?.getString("3ds_ip", "") ?: ""

    val pollMinutes: Long
        get() = settingsPrefs?.getString("poll_interval", "5")?.toLongOrNull() ?: 5

    private val settingsPrefs: SharedPreferences? by lazy {
        try {
            context.getSharedPreferences("widget_settings", Context.MODE_PRIVATE)
        } catch (_: Exception) { null }
    }

    fun fetchLead(ip: String): PokemonData? {
        if (ip.isBlank()) return null
        return try {
            val url = URL("http://$ip:8888/party/lead")
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 5000
            conn.readTimeout = 5000
            conn.requestMethod = "GET"

            if (conn.responseCode == 200) {
                val json = conn.inputStream.bufferedReader().readText()
                conn.disconnect()
                PokemonData.fromJson(json)
            } else {
                conn.disconnect()
                null
            }
        } catch (_: Exception) {
            null
        }
    }

    fun fetchParty(ip: String): PartyResponse? {
        if (ip.isBlank()) return null
        return try {
            val url = URL("http://$ip:8888/party")
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 5000
            conn.readTimeout = 5000
            conn.requestMethod = "GET"

            if (conn.responseCode == 200) {
                val json = conn.inputStream.bufferedReader().readText()
                conn.disconnect()
                PartyResponse.fromJson(json)
            } else {
                conn.disconnect()
                null
            }
        } catch (_: Exception) {
            null
        }
    }

    fun cacheLead(data: PokemonData) {
        prefs.edit()
            .putString("lead_json", data.toJson())
            .putLong("last_updated", System.currentTimeMillis())
            .apply()
    }

    fun getCachedLead(): PokemonData? {
        val json = prefs.getString("lead_json", null) ?: return null
        return try { PokemonData.fromJson(json) } catch (_: Exception) { null }
    }

    fun getLastUpdated(): Long = prefs.getLong("last_updated", 0)

    fun isStale(): Boolean {
        val last = getLastUpdated()
        if (last == 0L) return true
        return System.currentTimeMillis() - last > pollMinutes * 60 * 1000 * 2
    }

    fun fetchAndCacheSprite(ctx: Context, species: Int): Bitmap? {
        if (species <= 0) return null
        val file = File(ctx.cacheDir, "sprite_$species.png")
        // Use cached file if it exists
        if (file.exists() && file.length() > 0) {
            return BitmapFactory.decodeFile(file.absolutePath)
        }
        // Download from PokeAPI sprites
        return try {
            val url = URL("https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon/$species.png")
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 5000
            conn.readTimeout = 5000
            if (conn.responseCode == 200) {
                val bitmap = BitmapFactory.decodeStream(conn.inputStream)
                conn.disconnect()
                // Cache to disk
                if (bitmap != null) {
                    FileOutputStream(file).use { out ->
                        bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
                    }
                }
                bitmap
            } else {
                conn.disconnect()
                null
            }
        } catch (_: Exception) {
            null
        }
    }

    fun getCachedSprite(ctx: Context, species: Int): Bitmap? {
        if (species <= 0) return null
        val file = File(ctx.cacheDir, "sprite_$species.png")
        if (file.exists() && file.length() > 0) {
            return BitmapFactory.decodeFile(file.absolutePath)
        }
        return null
    }
}
