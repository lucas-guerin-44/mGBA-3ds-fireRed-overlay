package com.mgba.companion.walker

import android.content.Context
import android.content.Intent
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.app.AlertDialog
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.mgba.companion.R
import com.mgba.companion.data.PokemonRepository
import com.mgba.companion.data.Routes
import com.mgba.companion.data.SpeciesNames
import com.mgba.companion.data.WalkerStore
import com.mgba.companion.scanner.ScanActivity
import com.mgba.companion.worker.StepWorker

class WalkerActivity : AppCompatActivity(), SensorEventListener {

    private lateinit var store: WalkerStore
    private var sensorManager: SensorManager? = null
    private var stepSensor: Sensor? = null
    private var sensorRegistered = false

    /** Slot index that triggered the current pending scan/return launch. */
    private var pendingSlot = 0

    /** Inflated card views indexed by slot (0, 1, 2). */
    private val cardViews = arrayOfNulls<View>(WalkerStore.SLOT_COUNT)

    private val scanLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            initStepBaseline(pendingSlot)
            StepWorker.schedule(this)
        }
        refreshUI()
    }

    private val returnLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            if (!store.hasAnyActiveMon()) {
                StepWorker.cancel(this)
            }
        }
        refreshUI()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_walker)

        store = WalkerStore(this)
        sensorManager = getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        stepSensor = sensorManager?.getDefaultSensor(Sensor.TYPE_STEP_COUNTER)

        val container = findViewById<LinearLayout>(R.id.slots_container)
        val inflater = LayoutInflater.from(this)

        for (slot in 0 until WalkerStore.SLOT_COUNT) {
            val card = inflater.inflate(R.layout.layout_slot_card, container, false)
            cardViews[slot] = card
            container.addView(card)

            card.findViewById<TextView>(R.id.slot_label).text = "Slot ${slot + 1}"

            card.findViewById<Button>(R.id.btn_scan).setOnClickListener {
                pendingSlot = slot
                scanLauncher.launch(
                    Intent(this, ScanActivity::class.java)
                        .putExtra(ScanActivity.EXTRA_SLOT, slot)
                )
            }

            card.findViewById<Button>(R.id.btn_return).setOnClickListener {
                pendingSlot = slot
                returnLauncher.launch(
                    Intent(this, ReturnActivity::class.java)
                        .putExtra(ReturnActivity.EXTRA_SLOT, slot)
                )
            }

            card.findViewById<Button>(R.id.btn_change_route).setOnClickListener {
                showRoutePicker(slot)
            }
        }

        refreshUI()
    }

    override fun onResume() {
        super.onResume()
        if (store.hasAnyActiveMon()) {
            store.recalculateAllXp()
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
            val value = event.values[0].toInt()
            for (slot in 0 until WalkerStore.SLOT_COUNT) {
                val mon = store.getMonInSlot(slot) ?: continue
                if (mon.stepBaseline == 0) continue  // baseline not yet set, skip
                store.updateStepsForSlot(slot, value)
            }
            refreshUI()
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    private fun showRoutePicker(slot: Int) {
        val routes = Routes.all()
        val names: Array<CharSequence> = routes.map { "${it.name} — ${it.description}" }.toTypedArray()
        val mon = store.getMonInSlot(slot)
        val currentIdx = routes.indexOfFirst { it.key == mon?.routeKey }.coerceAtLeast(0)

        AlertDialog.Builder(this)
            .setTitle("Choose Route — Slot ${slot + 1}")
            .setSingleChoiceItems(names, currentIdx) { dialog, which ->
                store.setRouteForSlot(slot, routes[which].key)
                refreshUI()
                dialog.dismiss()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun initStepBaseline(slot: Int) {
        if (stepSensor == null) return
        sensorManager?.registerListener(object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent?) {
                if (event?.sensor?.type == Sensor.TYPE_STEP_COUNTER) {
                    val current = event.values[0].toInt()
                    val mon = store.getMonInSlot(slot)
                    if (mon != null && mon.stepBaseline == 0) {
                        val info = com.mgba.companion.data.Gen3Decoder.decode(mon.rawBlob)
                        if (info != null) {
                            store.storeMonInSlot(slot, mon.rawBlob, info, current)
                        }
                    }
                    sensorManager?.unregisterListener(this)
                }
            }
            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }, stepSensor, SensorManager.SENSOR_DELAY_FASTEST)
    }

    private fun refreshUI() {
        val repo = PokemonRepository(this)

        for (slot in 0 until WalkerStore.SLOT_COUNT) {
            val card = cardViews[slot] ?: continue
            val mon = store.getMonInSlot(slot)

            val emptyHint = card.findViewById<TextView>(R.id.card_empty_hint)
            val scanBtn = card.findViewById<Button>(R.id.btn_scan)
            val activeContent = card.findViewById<View>(R.id.card_active_content)
            val routeRow = card.findViewById<View>(R.id.card_route_row)
            val itemsView = card.findViewById<TextView>(R.id.slot_items)
            val returnBtn = card.findViewById<Button>(R.id.btn_return)

            if (mon == null) {
                emptyHint.visibility = View.VISIBLE
                scanBtn.visibility = View.VISIBLE
                activeContent.visibility = View.GONE
                routeRow.visibility = View.GONE
                itemsView.visibility = View.GONE
                card.findViewById<View>(R.id.card_divider).visibility = View.GONE
                returnBtn.visibility = View.GONE
                continue
            }

            emptyHint.visibility = View.GONE
            scanBtn.visibility = View.GONE
            activeContent.visibility = View.VISIBLE
            routeRow.visibility = View.VISIBLE
            itemsView.visibility = View.VISIBLE
            returnBtn.visibility = View.VISIBLE

            card.findViewById<TextView>(R.id.slot_nickname).text = mon.nickname
            card.findViewById<TextView>(R.id.slot_level).text = "Lv. ${mon.level}"
            card.findViewById<TextView>(R.id.slot_species).text = SpeciesNames.get(mon.species)
            card.findViewById<TextView>(R.id.slot_route).text = Routes.get(mon.routeKey).name
            card.findViewById<TextView>(R.id.slot_steps).text = "%,d".format(mon.totalSteps)
            card.findViewById<TextView>(R.id.slot_xp).text = "+${mon.bonusXp} XP"

            itemsView.text = if (mon.foundItems.isEmpty()) {
                "No items yet"
            } else {
                mon.foundItems.joinToString("  ·  ") { "${it.name} x${it.qty}" }
            }

            card.findViewById<View>(R.id.card_divider).visibility = View.VISIBLE

            val spriteView = card.findViewById<ImageView>(R.id.slot_sprite)
            val sprite = repo.getCachedSprite(this, mon.species)
            if (sprite != null) {
                spriteView.setImageBitmap(sprite)
            } else {
                Thread {
                    val fetched = repo.fetchAndCacheSprite(this, mon.species)
                    if (fetched != null) {
                        runOnUiThread { spriteView.setImageBitmap(fetched) }
                    }
                }.start()
            }
        }
    }
}
