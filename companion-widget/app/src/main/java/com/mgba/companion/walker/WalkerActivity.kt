package com.mgba.companion.walker

import android.content.Context
import android.content.Intent
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.mgba.companion.R
import com.mgba.companion.data.PokemonRepository
import com.mgba.companion.data.SpeciesNames
import com.mgba.companion.data.WalkerStore
import com.mgba.companion.scanner.ScanActivity
import com.mgba.companion.worker.StepWorker

class WalkerActivity : AppCompatActivity(), SensorEventListener {

    private lateinit var store: WalkerStore
    private var sensorManager: SensorManager? = null
    private var stepSensor: Sensor? = null
    private var sensorRegistered = false

    private val scanLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            // Mon received, set step baseline from current sensor and refresh
            initStepBaseline()
            StepWorker.schedule(this)
        }
        refreshUI()
    }

    private val returnLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            StepWorker.cancel(this)
        }
        refreshUI()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_walker)

        store = WalkerStore(this)
        sensorManager = getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        stepSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_STEP_COUNTER)

        findViewById<Button>(R.id.btn_scan).setOnClickListener {
            scanLauncher.launch(Intent(this, ScanActivity::class.java))
        }

        findViewById<Button>(R.id.btn_return).setOnClickListener {
            returnLauncher.launch(Intent(this, ReturnActivity::class.java))
        }

        refreshUI()
    }

    override fun onResume() {
        super.onResume()
        if (store.hasActiveMon()) {
            // Recalculate passive XP immediately
            store.recalculateXp()
            if (stepSensor != null) {
                sensorManager?.registerListener(this, stepSensor, SensorManager.SENSOR_DELAY_UI)
                sensorRegistered = true
            }
        }
        refreshUI()
    }

    override fun onPause() {
        super.onPause()
        if (sensorRegistered) {
            sensorManager?.unregisterListener(this)
            sensorRegistered = false
        }
    }

    override fun onSensorChanged(event: SensorEvent?) {
        if (event?.sensor?.type == Sensor.TYPE_STEP_COUNTER) {
            store.updateSteps(event.values[0].toInt())
            refreshUI()
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    private fun initStepBaseline() {
        // Read current step counter value and set as baseline
        if (stepSensor != null) {
            // We'll register briefly to get the current value
            // The StepWorker will also handle this on first run
            sensorManager?.registerListener(object : SensorEventListener {
                override fun onSensorChanged(event: SensorEvent?) {
                    if (event?.sensor?.type == Sensor.TYPE_STEP_COUNTER) {
                        val current = event.values[0].toInt()
                        val mon = store.getActiveMon()
                        if (mon != null && mon.stepBaseline == 0) {
                            // Re-store with proper baseline
                            val info = com.mgba.companion.data.Gen3Decoder.decode(mon.rawBlob)
                            if (info != null) {
                                store.storeMon(mon.rawBlob, info, current)
                            }
                        }
                        sensorManager?.unregisterListener(this)
                    }
                }
                override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
            }, stepSensor, SensorManager.SENSOR_DELAY_FASTEST)
        }
    }

    private fun refreshUI() {
        val emptyGroup = findViewById<View>(R.id.group_empty)
        val activeGroup = findViewById<View>(R.id.group_active)

        val mon = store.getActiveMon()
        if (mon == null) {
            emptyGroup.visibility = View.VISIBLE
            activeGroup.visibility = View.GONE
            return
        }

        emptyGroup.visibility = View.GONE
        activeGroup.visibility = View.VISIBLE

        findViewById<TextView>(R.id.walker_nickname).text = mon.nickname
        findViewById<TextView>(R.id.walker_level).text = "Lv. ${mon.level}"
        findViewById<TextView>(R.id.walker_species).text = SpeciesNames.get(mon.species)
        findViewById<TextView>(R.id.walker_steps).text = "${mon.totalSteps} steps"
        findViewById<TextView>(R.id.walker_xp).text = "+${mon.bonusXp} XP"

        val itemsView = findViewById<TextView>(R.id.walker_items)
        if (mon.foundItems.isEmpty()) {
            itemsView.text = "No items found yet"
        } else {
            itemsView.text = mon.foundItems.joinToString("\n") { "${it.name} x${it.qty}" }
        }

        // Load sprite
        val spriteView = findViewById<ImageView>(R.id.walker_sprite)
        val repo = PokemonRepository(this)
        val sprite = repo.getCachedSprite(this, mon.species)
        if (sprite != null) {
            spriteView.setImageBitmap(sprite)
        }
        // Also try to fetch in background if not cached
        if (sprite == null) {
            Thread {
                val fetched = repo.fetchAndCacheSprite(this, mon.species)
                if (fetched != null) {
                    runOnUiThread { spriteView.setImageBitmap(fetched) }
                }
            }.start()
        }
    }
}
