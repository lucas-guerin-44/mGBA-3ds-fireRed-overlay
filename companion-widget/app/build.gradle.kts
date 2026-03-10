plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mgba.companion"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.mgba.companion"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.work:work-runtime-ktx:2.10.0")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("com.google.android.material:material:1.12.0")

    // CameraX (QR scanning)
    implementation("androidx.camera:camera-core:1.4.1")
    implementation("androidx.camera:camera-camera2:1.4.1")
    implementation("androidx.camera:camera-lifecycle:1.4.1")
    implementation("androidx.camera:camera-view:1.4.1")

    // ML Kit barcode scanning
    implementation("com.google.mlkit:barcode-scanning:17.3.0")

    // ZXing (QR code generation)
    implementation("com.google.zxing:core:3.5.3")

    // Health Connect (step data from Mi Fit / Zepp and other fitness apps)
    implementation("androidx.health.connect:connect-client:1.1.0-alpha10")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    // Guava: resolve ListenableFuture conflict between Health Connect and CameraX
    implementation("com.google.guava:guava:32.1.3-android")
}
