package com.mgba.companion.worker

import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import androidx.work.*
import com.mgba.companion.data.PokemonRepository
import com.mgba.companion.widget.PokemonWidgetProvider
import java.util.concurrent.TimeUnit

class PokemonPollWorker(
    context: Context,
    params: WorkerParameters
) : Worker(context, params) {

    override fun doWork(): Result {
        val repo = PokemonRepository(applicationContext)
        val ip = repo.ip
        if (ip.isBlank()) return Result.failure()

        val data = repo.fetchLead(ip)
        if (data != null) {
            repo.cacheLead(data)
            repo.fetchAndCacheSprite(applicationContext, data.species)
        }

        // Update all widgets
        val manager = AppWidgetManager.getInstance(applicationContext)
        val ids = manager.getAppWidgetIds(
            ComponentName(applicationContext, PokemonWidgetProvider::class.java)
        )
        PokemonWidgetProvider.updateWidgets(applicationContext, manager, ids)

        return if (data != null) Result.success() else Result.retry()
    }

    companion object {
        private const val WORK_NAME = "pokemon_poll"

        fun schedule(context: Context, intervalMinutes: Long) {
            val request = PeriodicWorkRequestBuilder<PokemonPollWorker>(
                intervalMinutes, TimeUnit.MINUTES
            )
                .setConstraints(
                    Constraints.Builder()
                        .setRequiredNetworkType(NetworkType.CONNECTED)
                        .build()
                )
                .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 1, TimeUnit.MINUTES)
                .build()

            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                WORK_NAME,
                ExistingPeriodicWorkPolicy.UPDATE,
                request
            )
        }

        fun cancel(context: Context) {
            WorkManager.getInstance(context).cancelUniqueWork(WORK_NAME)
        }

        fun runOnce(context: Context) {
            val request = OneTimeWorkRequestBuilder<PokemonPollWorker>()
                .setConstraints(
                    Constraints.Builder()
                        .setRequiredNetworkType(NetworkType.CONNECTED)
                        .build()
                )
                .build()

            WorkManager.getInstance(context).enqueue(request)
        }
    }
}
