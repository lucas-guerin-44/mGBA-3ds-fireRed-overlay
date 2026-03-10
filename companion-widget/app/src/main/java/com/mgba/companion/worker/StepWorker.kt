package com.mgba.companion.worker

import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import androidx.work.*
import com.mgba.companion.data.Gen3Decoder
import com.mgba.companion.data.WalkerStore
import com.mgba.companion.widget.PokemonWidgetProvider
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Periodic background worker that reads the step counter sensor,
 * updates walk progress for all active slots, and refreshes widgets.
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

        val stepValue: Int
        if (stepSensor != null) {
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
            stepValue = raw
        } else {
            stepValue = -1
        }

        // Update every occupied slot
        for (slot in 0 until WalkerStore.SLOT_COUNT) {
            val mon = store.getMonInSlot(slot) ?: continue
            if (stepValue >= 0) {
                if (mon.stepBaseline == 0) {
                    // First reading for this slot — set baseline
                    val info = Gen3Decoder.decode(mon.rawBlob)
                    if (info != null) {
                        store.storeMonInSlot(slot, mon.rawBlob, info, stepValue)
                    }
                } else {
                    store.updateStepsForSlot(slot, stepValue)
                }
            } else {
                // No sensor — still recalculate passive XP
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
