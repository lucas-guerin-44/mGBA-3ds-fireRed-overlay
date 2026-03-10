package com.mgba.companion.data

import android.content.Context
import android.util.Log
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.records.StepsRecord
import androidx.health.connect.client.request.ReadRecordsRequest
import androidx.health.connect.client.time.TimeRangeFilter
import java.time.Instant

object HealthConnectHelper {

    fun isAvailable(context: Context): Boolean =
        HealthConnectClient.getSdkStatus(context) == HealthConnectClient.SDK_AVAILABLE

    /**
     * Returns total steps recorded between [sinceMs] and now, or -1 if Health Connect
     * is unavailable / the query fails.
     */
    suspend fun readStepsSince(context: Context, sinceMs: Long): Long {
        return try {
            val client = HealthConnectClient.getOrCreate(context)
            val response = client.readRecords(
                ReadRecordsRequest(
                    recordType = StepsRecord::class,
                    timeRangeFilter = TimeRangeFilter.between(
                        Instant.ofEpochMilli(sinceMs),
                        Instant.now()
                    )
                )
            )
            response.records.sumOf { it.count }
        } catch (e: Exception) {
            Log.e("HealthConnect", "readStepsSince failed: ${e::class.simpleName}: ${e.message}", e)
            -1L
        }
    }
}
