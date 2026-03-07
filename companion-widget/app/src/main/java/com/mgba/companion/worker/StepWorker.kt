package com.mgba.companion.worker

import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import androidx.work.*
import com.mgba.companion.data.WalkerStore
import com.mgba.companion.widget.PokemonWidgetProvider
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Periodic background worker that reads the step counter sensor,
 * updates walk progress, and refreshes widgets.
 */
class StepWorker(
    context: Context,
    params: WorkerParameters
) : Worker(context, params) {

    override fun doWork(): Result {
        val store = WalkerStore(applicationContext)
        if (!store.hasActiveMon()) return Result.success()

        // Read step counter sensor
        val sensorManager = applicationContext.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val stepSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_STEP_COUNTER)
        if (stepSensor == null) return Result.success()

        val latch = CountDownLatch(1)
        var stepValue = -1

        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent?) {
                if (event?.sensor?.type == Sensor.TYPE_STEP_COUNTER) {
                    stepValue = event.values[0].toInt()
                    sensorManager.unregisterListener(this)
                    latch.countDown()
                }
            }
            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }

        sensorManager.registerListener(listener, stepSensor, SensorManager.SENSOR_DELAY_FASTEST)
        latch.await(5, TimeUnit.SECONDS)
        sensorManager.unregisterListener(listener)

        if (stepValue >= 0) {
            // If baseline is 0, this is the first reading — set it
            val mon = store.getActiveMon()
            if (mon != null && mon.stepBaseline == 0) {
                val info = com.mgba.companion.data.Gen3Decoder.decode(mon.rawBlob)
                if (info != null) {
                    store.storeMon(mon.rawBlob, info, stepValue)
                }
            } else {
                store.updateSteps(stepValue)
            }
        } else {
            // No step sensor available — still recalculate for passive XP
            store.recalculateXp()
        }

        // Refresh widgets
        val manager = AppWidgetManager.getInstance(applicationContext)
        val ids = manager.getAppWidgetIds(
            ComponentName(applicationContext, PokemonWidgetProvider::class.java)
        )
        PokemonWidgetProvider.updateWidgets(applicationContext, manager, ids)

        return Result.success()
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
