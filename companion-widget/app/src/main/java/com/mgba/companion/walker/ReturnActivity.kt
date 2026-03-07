package com.mgba.companion.walker

import android.graphics.Bitmap
import android.graphics.Color
import android.os.Bundle
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.mgba.companion.R
import com.mgba.companion.data.WalkerStore

class ReturnActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_return)

        val store = WalkerStore(this)
        val mon = store.getActiveMon()
        val payload = store.buildReturnPayload()

        if (mon == null || payload == null) {
            Toast.makeText(this, "No active walker mon", Toast.LENGTH_SHORT).show()
            finish()
            return
        }

        // Display info
        val infoText = findViewById<TextView>(R.id.return_info)
        val itemsText = buildString {
            append("${mon.nickname} Lv.${mon.level}\n")
            append("Steps: ${mon.totalSteps}  |  XP: +${mon.bonusXp}\n")
            if (mon.foundItems.isNotEmpty()) {
                append("Items: ")
                append(mon.foundItems.joinToString(", ") { "${it.name} x${it.qty}" })
            }
        }
        infoText.text = itemsText

        // Generate QR code
        val qrImage = findViewById<ImageView>(R.id.return_qr)
        val bitmap = generateQr(payload)
        if (bitmap != null) {
            qrImage.setImageBitmap(bitmap)
        } else {
            Toast.makeText(this, "Failed to generate QR code", Toast.LENGTH_SHORT).show()
        }

        // Confirm return button
        findViewById<Button>(R.id.btn_confirm_return).setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Confirm Return")
                .setMessage("Has the 3DS scanned this QR code?")
                .setPositiveButton("Yes, mon returned") { _, _ ->
                    store.clearMon()
                    Toast.makeText(this, "${mon.nickname} returned to the game!", Toast.LENGTH_LONG).show()
                    setResult(RESULT_OK)
                    finish()
                }
                .setNegativeButton("Not yet", null)
                .show()
        }

        findViewById<Button>(R.id.btn_cancel_return).setOnClickListener {
            finish()
        }
    }

    private fun generateQr(text: String): Bitmap? {
        return try {
            val hints = mapOf(
                EncodeHintType.MARGIN to 2,
                EncodeHintType.CHARACTER_SET to "UTF-8"
            )
            val matrix = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, 512, 512, hints)
            val bitmap = Bitmap.createBitmap(matrix.width, matrix.height, Bitmap.Config.RGB_565)
            for (x in 0 until matrix.width) {
                for (y in 0 until matrix.height) {
                    bitmap.setPixel(x, y, if (matrix.get(x, y)) Color.BLACK else Color.WHITE)
                }
            }
            bitmap
        } catch (e: Exception) {
            null
        }
    }
}
