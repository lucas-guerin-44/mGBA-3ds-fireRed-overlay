package com.mgba.companion.data

import org.json.JSONArray
import org.json.JSONObject

private fun JSONArray.toIntList(): List<Int> {
    val list = mutableListOf<Int>()
    for (i in 0 until length()) list.add(getInt(i))
    return list
}

data class PokemonData(
    val species: Int,
    val speciesName: String,
    val nickname: String,
    val level: Int,
    val hp: Int,
    val maxHp: Int,
    val atk: Int,
    val def: Int,
    val spa: Int,
    val spd: Int,
    val spe: Int,
    val moves: List<Int>,
    val pp: List<Int>,
    val status: String?
) {
    fun toJson(): String = JSONObject().apply {
        put("species", species)
        put("speciesName", speciesName)
        put("nickname", nickname)
        put("level", level)
        put("hp", hp)
        put("maxHp", maxHp)
        put("atk", atk)
        put("def", def)
        put("spa", spa)
        put("spd", spd)
        put("spe", spe)
        put("status", status ?: JSONObject.NULL)
    }.toString()

    companion object {
        fun fromJson(json: String): PokemonData {
            val obj = JSONObject(json)
            return PokemonData(
                species = obj.getInt("species"),
                speciesName = obj.getString("speciesName"),
                nickname = obj.getString("nickname"),
                level = obj.getInt("level"),
                hp = obj.getInt("hp"),
                maxHp = obj.getInt("maxHp"),
                atk = obj.optInt("atk"),
                def = obj.optInt("def"),
                spa = obj.optInt("spa"),
                spd = obj.optInt("spd"),
                spe = obj.optInt("spe"),
                moves = obj.optJSONArray("moves")?.let { arr ->
                    (0 until arr.length()).map { arr.getInt(it) }
                } ?: emptyList(),
                pp = obj.optJSONArray("pp")?.let { arr ->
                    (0 until arr.length()).map { arr.getInt(it) }
                } ?: emptyList(),
                status = obj.optString("status").takeIf { it != "null" && it.isNotEmpty() }
            )
        }
    }
}

data class PartyResponse(
    val partyCount: Int,
    val party: List<PokemonData>
) {
    companion object {
        fun fromJson(json: String): PartyResponse {
            val obj = JSONObject(json)
            val arr = obj.getJSONArray("party")
            val party = (0 until arr.length()).map { PokemonData.fromJson(arr.getJSONObject(it).toString()) }
            return PartyResponse(obj.getInt("partyCount"), party)
        }
    }
}
