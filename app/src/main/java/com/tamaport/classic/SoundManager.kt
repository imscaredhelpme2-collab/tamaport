package com.tamaport.classic

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.SoundPool
import android.util.Log
import java.io.File

/**
 * Port of the original sound manager (16 sound slots).
 * Slots 0-3 and 15 are streamed (music / jingle), slots 4-14 are short effects played through SoundPool.
 */
class SoundManager(private val ctx: Context) {
    private class Def(val streamed: Boolean, val loop: Boolean, val name: String)

    private val defs = arrayOf(
        Def(true, true, "title"), Def(true, true, "gameplay"), Def(true, true, "imadokiminigame"), Def(true, true, "imadokiminigame"),
        Def(false, false, "gameend"), Def(false, false, "gameopen"), Def(false, true, "gameplay_se"), Def(false, false, "henshin"),
        Def(false, false, "okori"), Def(false, false, "reset"), Def(false, false, "shibou"), Def(false, false, "sousa"),
        Def(false, false, "tanjou"), Def(false, false, "wagamama_shitsuke"), Def(false, false, "yobidashi"), Def(true, false, "yorokobi.wav"))

    private val players = arrayOfNulls<MediaPlayer>(defs.size)
    private val resumeAfterPause = BooleanArray(defs.size)
    private val soundIds = IntArray(defs.size)
    private val streamIds = IntArray(defs.size)
    private val volume = FloatArray(defs.size) { 1f }
    private var paused = false
    private var pool: SoundPool? = null

    fun init() {
        val attrs = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()
        val p = SoundPool.Builder().setMaxStreams(16).setAudioAttributes(attrs).build()
        pool = p
        for (i in defs.indices) {
            val d = defs[i]
            if (d.streamed) continue
            val path = assetToFile("sound_se", d.name) ?: continue
            soundIds[i] = p.load(path, 1)
        }
    }

    fun play(i: Int) {
        if (paused || i !in defs.indices) return
        val d = defs[i]
        try {
            if (d.streamed) {
                players[i]?.let { if (it.isPlaying) { it.seekTo(0); return } else { it.release(); players[i] = null } }
                val path = assetToFile("sound_bgm", d.name) ?: return
                val mp = MediaPlayer()
                mp.setDataSource(path)
                mp.isLooping = d.loop
                mp.prepare()
                mp.setVolume(volume[i], volume[i])
                mp.start()
                players[i] = mp
            } else {
                val id = soundIds[i]
                if (id == 0) return
                streamIds[i] = pool?.play(id, volume[i], volume[i], 1, if (d.loop) -1 else 0, 1f) ?: 0
            }
        } catch (e: Exception) { Log.e(TAG, "play($i): $e") }
    }

    fun stop(i: Int) {
        if (i !in defs.indices) return
        try {
            if (defs[i].streamed) { players[i]?.let { it.stop(); it.release() }; players[i] = null }
            else if (streamIds[i] != 0) pool?.stop(streamIds[i])
        } catch (e: Exception) { Log.e(TAG, "stop($i): $e") }
    }

    /** True while a streamed sound is still playing (the engine polls this to detect the end of a jingle). */
    fun isPlaying(i: Int): Boolean = try { i in defs.indices && defs[i].streamed && players[i]?.isPlaying == true } catch (e: Exception) { false }

    fun setVolume(all: Float) = setRange(0, defs.size, all)
    fun setBgmVolume(v: Float) = setRange(0, 4, v)
    fun setSeVolume(v: Float) = setRange(4, defs.size, v)

    private fun setRange(from: Int, to: Int, v: Float) {
        for (i in from until to) {
            volume[i] = v
            try {
                if (defs[i].streamed) players[i]?.setVolume(v, v)
                else if (streamIds[i] != 0) pool?.setVolume(streamIds[i], v, v)
            } catch (e: Exception) { /* player may be in a transient state */ }
        }
    }

    fun pauseAll() {
        paused = true
        for (i in defs.indices) {
            try { val p = players[i]; resumeAfterPause[i] = p?.isPlaying == true; if (resumeAfterPause[i]) p?.pause() } catch (e: Exception) { resumeAfterPause[i] = false }
        }
        pool?.autoPause()
    }

    fun resumeAll() {
        paused = false
        for (i in defs.indices) {
            try { if (resumeAfterPause[i]) players[i]?.start() } catch (e: Exception) { /* ignore */ }
            resumeAfterPause[i] = false
        }
        pool?.autoResume()
    }

    fun release() {
        for (i in defs.indices) { try { players[i]?.release() } catch (e: Exception) { }; players[i] = null }
        pool?.release(); pool = null
    }

    /** SoundPool/MediaPlayer need a real file or an uncompressed asset, so copy assets out once (as the original did). */
    private fun assetToFile(dir: String, name: String): String? {
        return try {
            val f = File(ctx.filesDir, "snd/$dir/$name")
            if (!f.exists() || f.length() == 0L) {
                f.parentFile?.mkdirs()
                ctx.assets.open("$dir/$name").use { input -> f.outputStream().use { input.copyTo(it) } }
            }
            f.absolutePath
        } catch (e: Exception) { Log.e(TAG, "missing sound asset $dir/$name: $e"); null }
    }

    companion object { private const val TAG = "SoundManager" }
}
