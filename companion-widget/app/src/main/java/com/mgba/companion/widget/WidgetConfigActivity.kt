package com.mgba.companion.widget

import android.appwidget.AppWidgetManager
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.mgba.companion.R
import com.mgba.companion.worker.PokemonPollWorker

class WidgetConfigActivity : AppCompatActivity() {

    private var appWidgetId = AppWidgetManager.INVALID_APPWIDGET_ID

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setResult(RESULT_CANCELED)

        appWidgetId = intent?.extras?.getInt(
            AppWidgetManager.EXTRA_APPWIDGET_ID,
            AppWidgetManager.INVALID_APPWIDGET_ID
        ) ?: AppWidgetManager.INVALID_APPWIDGET_ID

        if (appWidgetId == AppWidgetManager.INVALID_APPWIDGET_ID) {
            finish()
            return
        }

        setContentView(R.layout.activity_config)

        val ipField = findViewById<EditText>(R.id.ip_input)
        val intervalSpinner = findViewById<Spinner>(R.id.interval_spinner)
        val saveBtn = findViewById<Button>(R.id.save_btn)

        val intervals = arrayOf("5", "10", "15", "30", "60")
        val labels = arrayOf("5 min", "10 min", "15 min", "30 min", "60 min")
        intervalSpinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, labels)

        // Load existing settings
        val prefs = getSharedPreferences("widget_settings", MODE_PRIVATE)
        ipField.setText(prefs.getString("3ds_ip", ""))
        val savedInterval = prefs.getString("poll_interval", "5") ?: "5"
        val idx = intervals.indexOf(savedInterval)
        if (idx >= 0) intervalSpinner.setSelection(idx)

        // Skip config — just create the widget (walker-only mode)
        findViewById<Button>(R.id.skip_btn).setOnClickListener {
            val result = Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, appWidgetId)
            setResult(RESULT_OK, result)
            finish()
        }

        saveBtn.setOnClickListener {
            val ip = ipField.text.toString().trim()
            if (ip.isBlank()) {
                Toast.makeText(this, "Enter 3DS IP address", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }

            val intervalValue = intervals[intervalSpinner.selectedItemPosition]

            prefs.edit()
                .putString("3ds_ip", ip)
                .putString("poll_interval", intervalValue)
                .apply()

            // Schedule polling
            PokemonPollWorker.schedule(this, intervalValue.toLong())

            // Trigger immediate poll
            PokemonPollWorker.runOnce(this)

            // Confirm widget creation
            val result = Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, appWidgetId)
            setResult(RESULT_OK, result)
            finish()
        }
    }
}
