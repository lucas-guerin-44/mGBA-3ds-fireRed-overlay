package com.mgba.companion.data

object Routes {

    data class LootEntry(val itemId: Int, val name: String, val weight: Int)

    data class Route(
        val key: String,
        val name: String,
        val description: String,
        val loot: List<LootEntry>,
        val lootStepInterval: Int = 500
    )

    val DEFAULT = Route(
        key = "default",
        name = "Route 1",
        description = "A quiet path with common supplies.",
        loot = listOf(
            LootEntry(13, "Potion", 30),
            LootEntry(22, "Super Potion", 15),
            LootEntry(4, "Poke Ball", 25),
            LootEntry(139, "Oran Berry", 15),
            LootEntry(133, "Cheri Berry", 10),
            LootEntry(68, "Rare Candy", 5),
        )
    )

    private val ROUTE_LIST = listOf(
        DEFAULT,
        Route(
            key = "mt_moon",
            name = "Mt. Moon",
            description = "Dark caves with rare minerals.",
            loot = listOf(
                LootEntry(13, "Potion", 20),
                LootEntry(93, "Moon Stone", 8),
                LootEntry(4, "Poke Ball", 15),
                LootEntry(23, "Hyper Potion", 10),
                LootEntry(68, "Rare Candy", 8),
                LootEntry(85, "Repel", 20),
                LootEntry(86, "Escape Rope", 19),
            ),
            lootStepInterval = 600
        ),
        Route(
            key = "safari_zone",
            name = "Safari Zone",
            description = "Tall grass and exotic finds.",
            loot = listOf(
                LootEntry(5, "Great Ball", 20),
                LootEntry(6, "Ultra Ball", 5),
                LootEntry(139, "Oran Berry", 20),
                LootEntry(133, "Cheri Berry", 15),
                LootEntry(134, "Rawst Berry", 15),
                LootEntry(68, "Rare Candy", 8),
                LootEntry(22, "Super Potion", 17),
            ),
            lootStepInterval = 450
        ),
        Route(
            key = "power_plant",
            name = "Power Plant",
            description = "Electrifying, with technical gear.",
            loot = listOf(
                LootEntry(23, "Hyper Potion", 15),
                LootEntry(22, "Super Potion", 15),
                LootEntry(85, "Repel", 15),
                LootEntry(5, "Great Ball", 15),
                LootEntry(68, "Rare Candy", 10),
                LootEntry(55, "PP Up", 5),
                LootEntry(86, "Escape Rope", 25),
            ),
            lootStepInterval = 550
        ),
        Route(
            key = "victory_road",
            name = "Victory Road",
            description = "Treacherous but rewarding.",
            loot = listOf(
                LootEntry(23, "Hyper Potion", 15),
                LootEntry(6, "Ultra Ball", 10),
                LootEntry(68, "Rare Candy", 15),
                LootEntry(55, "PP Up", 8),
                LootEntry(5, "Great Ball", 15),
                LootEntry(22, "Super Potion", 12),
                LootEntry(86, "Escape Rope", 25),
            ),
            lootStepInterval = 700
        ),
    )

    private val BY_KEY = ROUTE_LIST.associateBy { it.key }

    fun all(): List<Route> = ROUTE_LIST

    fun get(key: String?): Route = BY_KEY[key] ?: DEFAULT
}
