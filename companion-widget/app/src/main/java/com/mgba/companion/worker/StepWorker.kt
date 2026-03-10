package com.mgba.companion.worker

import android.app.NotificationChannel
import android.app.NotificationManager
import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import androidx.core.app.NotificationCompat
import androidx.work.*
import com.mgba.companion.data.HealthConnectHelper
import com.mgba.companion.data.WalkerStore
import com.mgba.companion.widget.PokemonWidgetProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Periodic background worker that reads the step counter sensor and Health Connect,
 * updates walk progress for all active slots, and refreshes widgets.
 */
class StepWorker(
    context: Context,
    params: WorkerParameters
) : CoroutineWorker(context, params) {

    override suspend fun doWork(): Result {
        val store = WalkerStore(applicationContext)
        if (!store.hasActiveMon()) return Result.success()

        setForeground(createForegroundInfo())

        val stepValue = readSensorSteps()
        val hcAvailable = HealthConnectHelper.isAvailable(applicationContext)

        for (slot in 0 until WalkerStore.SLOT_COUNT) {
            val mon = store.getMonInSlot(slot) ?: continue

            // Health Connect: query steps since this mon was sent out
            val hcSteps: Long = if (hcAvailable) {
                HealthConnectHelper.readStepsSince(applicationContext, mon.sentAt)
            } else -1L

            if (stepValue >= 0) {
                if (mon.stepBaseline == 0) {
                    // First sensor reading — set baseline without resetting steps/items/sentAt
                    store.setStepBaselineForSlot(slot, stepValue)
                } else {
                    val sensorTotal = (stepValue - mon.stepBaseline).coerceAtLeast(0)
                    val best = if (hcSteps > sensorTotal) hcSteps.toInt() else sensorTotal
                    store.updateStepsDirectlyForSlot(slot, best)
                }
            } else if (hcSteps >= 0) {
                // No hardware sensor — use Health Connect alone
                store.updateStepsDirectlyForSlot(slot, hcSteps.toInt())
            } else {
                store.recalculateXpForSlot(slot)
            }
        }

        // Refresh widgets
        val manager = AppWidgetManager.getInstance(applicationContext)
        val ids = manager.getAppWidgetIds(
            ComponentName(applicationContext, PokemonWidgetProvider::class.java)
        )
        PokemonWidgetProvider.updateWidgets(applicationContext, manager, ids)

        return Result.success()
    }

    private suspend fun readSensorSteps(): Int = withContext(Dispatchers.IO) {
        val sensorManager = applicationContext.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val stepSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_STEP_COUNTER)
            ?: return@withContext -1

        val latch = CountDownLatch(1)
        var raw = -1

        val handlerThread = HandlerThread("step-sensor-thread")
        handlerThread.start()
        val handler = Handler(handlerThread.looper)

        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent?) {
                if (event?.sensor?.type == Sensor.TYPE_STEP_COUNTER) {
                    raw = event.values[0].toInt()
                    sensorManager.unregisterListener(this)
                    latch.countDown()
                }
            }
            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }

        sensorManager.registerListener(listener, stepSensor, SensorManager.SENSOR_DELAY_FASTEST, handler)
        latch.await(5, TimeUnit.SECONDS)
        sensorManager.unregisterListener(listener)
        handlerThread.quit()
        raw
    }

    private fun createForegroundInfo(): ForegroundInfo {
        val channelId = "walker_steps_channel"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                channelId, "Walker Steps", NotificationManager.IMPORTANCE_LOW
            )
            applicationContext.getSystemService(NotificationManager::class.java)
                ?.createNotificationChannel(channel)
        }
        val notification = NotificationCompat.Builder(applicationContext, channelId)
            .setContentTitle("Counting steps…")
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setOngoing(true)
            .build()
        return ForegroundInfo(1, notification)
    }

    companion object {
        private const val WORK_NAME = "walker_steps"

        fun schedule(context: Context) {
            val request = PeriodicWorkRequestBuilder<StepWorker>(
                15, TimeUnit.MINUTES
            )
                .setConstraints(Constraints.Builder().build())
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
            val request = OneTimeWorkRequestBuilder<StepWorker>().build()
            WorkManager.getInstance(context).enqueue(request)
        }
    }
}
