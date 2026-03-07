package com.mgba.companion.scanner

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Base64
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import com.mgba.companion.R
import com.mgba.companion.data.Gen3Decoder
import com.mgba.companion.data.WalkerStore
import java.util.concurrent.Executors

class ScanActivity : AppCompatActivity() {

    private var scanned = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_scan)

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA), 100)
        } else {
            startCamera()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 100 && grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
            startCamera()
        } else {
            Toast.makeText(this, "Camera permission required", Toast.LENGTH_SHORT).show()
            finish()
        }
    }

    private fun startCamera() {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(this)
        cameraProviderFuture.addListener({
            val cameraProvider = cameraProviderFuture.get()
            val preview = Preview.Builder().build().also {
                it.surfaceProvider = findViewById<PreviewView>(R.id.preview_view).surfaceProvider
            }

            val analysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build()

            analysis.setAnalyzer(Executors.newSingleThreadExecutor()) { imageProxy ->
                processImage(imageProxy)
            }

            cameraProvider.unbindAll()
            cameraProvider.bindToLifecycle(
                this, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis
            )
        }, ContextCompat.getMainExecutor(this))
    }

    @androidx.camera.core.ExperimentalGetImage
    private fun processImage(imageProxy: ImageProxy) {
        if (scanned) {
            imageProxy.close()
            return
        }

        val mediaImage = imageProxy.image
        if (mediaImage == null) {
            imageProxy.close()
            return
        }

        val inputImage = InputImage.fromMediaImage(mediaImage, imageProxy.imageInfo.rotationDegrees)
        val scanner = BarcodeScanning.getClient()

        scanner.process(inputImage)
            .addOnSuccessListener { barcodes ->
                for (barcode in barcodes) {
                    if (barcode.valueType == Barcode.TYPE_TEXT) {
                        val text = barcode.rawValue ?: continue
                        if (text.startsWith("PK2:SEND:")) {
                            scanned = true
                            handleSendPayload(text)
                            return@addOnSuccessListener
                        }
                    }
                }
            }
            .addOnCompleteListener {
                imageProxy.close()
            }
    }

    private fun handleSendPayload(payload: String) {
        // Format: PK2:SEND:<8-char hex OTID>:<base64 of 100-byte party slot>
        val rest = payload.removePrefix("PK2:SEND:")
        val colonIdx = rest.indexOf(':')
        if (colonIdx < 0) {
            runOnUiThread {
                Toast.makeText(this, "Invalid payload format", Toast.LENGTH_SHORT).show()
                scanned = false
            }
            return
        }

        val b64 = rest.substring(colonIdx + 1)
        val blob: ByteArray
        try {
            blob = Base64.decode(b64, Base64.NO_WRAP)
        } catch (e: Exception) {
            runOnUiThread {
                Toast.makeText(this, "Invalid QR data", Toast.LENGTH_SHORT).show()
                scanned = false
            }
            return
        }

        if (blob.size < 100) {
            runOnUiThread {
                Toast.makeText(this, "Bad blob size: ${blob.size} (need at least 100)", Toast.LENGTH_SHORT).show()
                scanned = false
            }
            return
        }

        val partySlot = if (blob.size > 100) blob.copyOfRange(0, 100) else blob
        val info = Gen3Decoder.decode(partySlot)
        if (info == null) {
            runOnUiThread {
                Toast.makeText(this, "Could not decode Pokemon data", Toast.LENGTH_SHORT).show()
                scanned = false
            }
            return
        }

        val store = WalkerStore(this)
        if (store.hasActiveMon()) {
            runOnUiThread {
                Toast.makeText(this, "Already walking a Pokemon! Return it first.", Toast.LENGTH_LONG).show()
                scanned = false
            }
            return
        }

        // Step baseline: 0 for now, StepWorker will set the real baseline on first run
        store.storeMon(partySlot, info, 0)

        runOnUiThread {
            Toast.makeText(this, "Received ${info.nickname} (Lv.${info.level})!", Toast.LENGTH_LONG).show()
            setResult(RESULT_OK)
            finish()
        }
    }
}
