package com.mgba.companion.data

/**
 * Decodes Gen 3 Pokemon data from a raw 100-byte party slot blob.
 * Matches the C implementation in overlay.c.
 */
object Gen3Decoder {

    // Substructure order table: index = PID % 24
    // 0=Growth, 1=Attacks, 2=EVs, 3=Misc
    private val SUBSTRUCT_ORDER = arrayOf(
        intArrayOf(0,1,2,3), intArrayOf(0,1,3,2),
        intArrayOf(0,2,1,3), intArrayOf(0,2,3,1),
        intArrayOf(0,3,1,2), intArrayOf(0,3,2,1),
        intArrayOf(1,0,2,3), intArrayOf(1,0,3,2),
        intArrayOf(1,2,0,3), intArrayOf(1,2,3,0),
        intArrayOf(1,3,0,2), intArrayOf(1,3,2,0),
        intArrayOf(2,0,1,3), intArrayOf(2,0,3,1),
        intArrayOf(2,1,0,3), intArrayOf(2,1,3,0),
        intArrayOf(2,3,0,1), intArrayOf(2,3,1,0),
        intArrayOf(3,0,1,2), intArrayOf(3,0,2,1),
        intArrayOf(3,1,0,2), intArrayOf(3,1,2,0),
        intArrayOf(3,2,0,1), intArrayOf(3,2,1,0),
    )

    data class MonInfo(
        val species: Int,
        val nickname: String,
        val level: Int,
        val pid: Long,
        val otid: Long
    )

    fun decode(blob: ByteArray): MonInfo? {
        if (blob.size < 100) return null

        val pid = readU32LE(blob, 0x00)
        val otid = readU32LE(blob, 0x04)
        val nickname = decodeGen3String(blob, 0x08, 10)
        val level = blob[0x54].toInt() and 0xFF

        val key = pid xor otid
        val decrypted = decryptSubstructs(blob, key)

        val growthOffset = findSubstructOffset(pid, 0)
        val species = readU16LE(decrypted, growthOffset)

        if (species == 0 || species > 440) return null

        return MonInfo(species, nickname, level, pid, otid)
    }

    fun decodeGen3Char(b: Int): Char {
        val c = b and 0xFF
        return when {
            c in 0xBB..0xD4 -> ('A' + (c - 0xBB))
            c in 0xD5..0xEE -> ('a' + (c - 0xD5))
            c in 0xA1..0xAA -> ('0' + (c - 0xA1))
            c == 0x00 -> ' '
            c == 0xAB -> '!'
            c == 0xAC -> '?'
            c == 0xAD -> '.'
            c == 0xAE -> '-'
            c == 0xB8 -> ','
            c == 0xBA -> '/'
            c == 0xFF -> '\u0000'
            else -> ' '
        }
    }

    fun decodeGen3String(bytes: ByteArray, offset: Int, maxLen: Int): String {
        val sb = StringBuilder()
        for (i in 0 until maxLen) {
            if (offset + i >= bytes.size) break
            val ch = decodeGen3Char(bytes[offset + i].toInt())
            if (ch == '\u0000') break
            sb.append(ch)
        }
        return sb.toString().trim()
    }

    private fun readU16LE(bytes: ByteArray, offset: Int): Int {
        return (bytes[offset].toInt() and 0xFF) or
               ((bytes[offset + 1].toInt() and 0xFF) shl 8)
    }

    private fun readU32LE(bytes: ByteArray, offset: Int): Long {
        return (bytes[offset].toInt() and 0xFF).toLong() or
               ((bytes[offset + 1].toInt() and 0xFF).toLong() shl 8) or
               ((bytes[offset + 2].toInt() and 0xFF).toLong() shl 16) or
               ((bytes[offset + 3].toInt() and 0xFF).toLong() shl 24)
    }

    private fun findSubstructOffset(pid: Long, which: Int): Int {
        val order = (pid % 24).toInt()
        for (pos in 0..3) {
            if (SUBSTRUCT_ORDER[order][pos] == which) {
                return pos * 12
            }
        }
        return 0
    }

    private fun decryptSubstructs(blob: ByteArray, key: Long): ByteArray {
        val out = ByteArray(48)
        for (i in 0 until 12) {
            val offset = 0x20 + i * 4
            val enc = readU32LE(blob, offset)
            val dec = enc xor key
            out[i * 4] = (dec and 0xFF).toByte()
            out[i * 4 + 1] = ((dec shr 8) and 0xFF).toByte()
            out[i * 4 + 2] = ((dec shr 16) and 0xFF).toByte()
            out[i * 4 + 3] = ((dec shr 24) and 0xFF).toByte()
        }
        return out
    }
}
