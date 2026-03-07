package com.mgba.companion.widget

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.RemoteViews
import com.mgba.companion.R
import com.mgba.companion.data.PokemonData
import com.mgba.companion.data.PokemonRepository
import com.mgba.companion.data.SpeciesNames
import com.mgba.companion.data.Routes
import com.mgba.companion.data.WalkerStore
import com.mgba.companion.walker.WalkerActivity
import com.mgba.companion.worker.PokemonPollWorker
import java.text.SimpleDateFormat
import java.util.*

class PokemonWidgetProvider : AppWidgetProvider() {

    override fun onReceive(context: Context, intent: Intent) {
        super.onReceive(context, intent)
        if (intent.action == "com.mgba.companion.REFRESH") {
            PokemonPollWorker.runOnce(context)
        }
    }

    override fun onUpdate(
        context: Context,
        appWidgetManager: AppWidgetManager,
        appWidgetIds: IntArray
    ) {
        for (id in appWidgetIds) {
            val opts = appWidgetManager.getAppWidgetOptions(id)
            updateWidget(context, appWidgetManager, id, opts)
        }
        PokemonPollWorker.runOnce(context)
    }

    override fun onAppWidgetOptionsChanged(
        context: Context,
        appWidgetManager: AppWidgetManager,
        appWidgetId: Int,
        newOptions: Bundle
    ) {
        updateWidget(context, appWidgetManager, appWidgetId, newOptions)
    }

    override fun onEnabled(context: Context) {
        val repo = PokemonRepository(context)
        PokemonPollWorker.schedule(context, repo.pollMinutes)
    }

    override fun onDisabled(context: Context) {
        PokemonPollWorker.cancel(context)
    }

    companion object {
        // Width thresholds in dp
        private const val SHOW_INFO = 120   // ~2 cells
        private const val SHOW_EXTRA = 200  // ~3 cells

        fun updateWidgets(
            context: Context,
            manager: AppWidgetManager,
            ids: IntArray
        ) {
            for (id in ids) {
                val opts = manager.getAppWidgetOptions(id)
                updateWidget(context, manager, id, opts)
            }
        }

        private fun updateWidget(
            context: Context,
            manager: AppWidgetManager,
            id: Int,
            opts: Bundle
        ) {
            val widthDp = opts.getInt(AppWidgetManager.OPTION_APPWIDGET_MIN_WIDTH, 300)
            val repo = PokemonRepository(context)
            val walkerStore = WalkerStore(context)

            // Walker mon takes priority over HTTP-polled lead
            val walkerMon = walkerStore.getActiveMon()
            val views = if (walkerMon != null) {
                buildWalkerViews(context, repo, walkerMon, widthDp)
            } else {
                val lead = repo.getCachedLead()
                val lastUpdated = repo.getLastUpdated()
                val isStale = repo.isStale()
                buildLeadViews(context, repo, lead, lastUpdated, isStale, widthDp)
            }
            manager.updateAppWidget(id, views)
        }

        private fun buildBaseViews(context: Context, widthDp: Int): Triple<RemoteViews, Boolean, Boolean> {
            val views = RemoteViews(context.packageName, R.layout.widget_pokemon)

            // Tap widget to open WalkerActivity
            val openIntent = Intent(context, WalkerActivity::class.java)
            val pendingIntent = PendingIntent.getActivity(
                context, 0, openIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            views.setOnClickPendingIntent(R.id.widget_root, pendingIntent)

            val showInfo = widthDp >= SHOW_INFO
            val showExtra = widthDp >= SHOW_EXTRA
            views.setViewVisibility(R.id.info_column, if (showInfo) View.VISIBLE else View.GONE)
            views.setViewVisibility(R.id.pokemon_extra, if (showExtra) View.VISIBLE else View.GONE)

            return Triple(views, showInfo, showExtra)
        }

        private fun buildWalkerViews(
            context: Context,
            repo: PokemonRepository,
            mon: WalkerStore.WalkerMon,
            widthDp: Int
        ): RemoteViews {
            // Recalculate XP so widget shows fresh passive gains
            val store = WalkerStore(context)
            store.recalculateXp()
            val freshMon = store.getActiveMon() ?: mon

            val (views, showInfo, showExtra) = buildBaseViews(context, widthDp)

            val sprite = repo.getCachedSprite(context, freshMon.species)
            if (sprite != null) {
                views.setImageViewBitmap(R.id.pokemon_sprite, sprite)
            }

            if (!showInfo) return views

            views.setTextViewText(R.id.pokemon_name, freshMon.nickname)
            views.setTextViewText(R.id.pokemon_info,
                "${freshMon.totalSteps} steps · +${freshMon.bonusXp} XP")
            views.setTextColor(R.id.pokemon_info, android.graphics.Color.parseColor("#40FF40"))

            if (!showExtra) return views

            val parts = mutableListOf<String>()
            parts.add(Routes.get(freshMon.routeKey).name)
            parts.add("Lv. ${freshMon.level} ${SpeciesNames.get(freshMon.species)}")
            if (freshMon.foundItems.isNotEmpty()) {
                parts.add(freshMon.foundItems.joinToString(", ") {
                    "${it.name}${if (it.qty > 1) " x${it.qty}" else ""}"
                })
            }
            views.setTextViewText(R.id.pokemon_extra, parts.joinToString(" · "))

            return views
        }

        private fun buildLeadViews(
            context: Context,
            repo: PokemonRepository,
            lead: PokemonData?,
            lastUpdated: Long,
            isStale: Boolean,
            widthDp: Int
        ): RemoteViews {
            val (views, showInfo, showExtra) = buildBaseViews(context, widthDp)

            if (lead == null) {
                if (showInfo) {
                    views.setTextViewText(R.id.pokemon_name, "No data")
                    views.setTextViewText(R.id.pokemon_info, "Tap to open")
                    views.setTextViewText(R.id.pokemon_extra, "")
                }
                return views
            }

            val sprite = repo.getCachedSprite(context, lead.species)
            if (sprite != null) {
                views.setImageViewBitmap(R.id.pokemon_sprite, sprite)
            }

            if (!showInfo) return views

            views.setTextViewText(R.id.pokemon_name, lead.nickname)

            val hpPct = if (lead.maxHp > 0) (lead.hp * 100 / lead.maxHp) else 0
            val hpColor = when {
                hpPct > 50 -> "#40FF40"
                hpPct > 25 -> "#FFFF00"
                else -> "#FF4040"
            }
            views.setTextViewText(R.id.pokemon_info, "Lv. ${lead.level} · ${hpPct}% HP")
            views.setTextColor(R.id.pokemon_info, android.graphics.Color.parseColor(hpColor))

            if (!showExtra) return views

            val parts = mutableListOf<String>()
            if (lead.nickname != lead.speciesName) {
                parts.add(lead.speciesName)
            }
            lead.status?.let { parts.add(it) }
            val staleMarker = if (isStale) " (stale)" else ""
            val timeStr = if (lastUpdated > 0) {
                SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(lastUpdated))
            } else "never"
            parts.add("$timeStr$staleMarker")
            views.setTextViewText(R.id.pokemon_extra, parts.joinToString(" · "))

            return views
        }
    }
}
