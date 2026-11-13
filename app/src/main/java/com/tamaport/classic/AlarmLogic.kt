package com.tamaport.classic

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import java.io.File
import java.util.Calendar

/**
 * "Tamagotchi is calling you!" alerts.
 * The engine writes a schedule file (7-byte records: id, yy, mm, dd, hh, mi, ss); this is a port of the original
 * shell's logic that turns it into one wake-up alarm at a time plus a notification when a call time has passed.
 */
object AlarmLogic {
    private const val TAG = "AlarmLogic"
    const val INFO_FILE = "TMCG_infoFile"
    private const val CHANNEL = "tama_call"
    private const val NOTIF_ID = 1001

    fun deleteInfo(ctx: Context) { ctx.deleteFile(INFO_FILE) }

    /** Original `aj.a(ctx, cls, ms)`: one alarm, `delayMs` from now. */
    fun scheduleIn(ctx: Context, delayMs: Int) {
        val cal = Calendar.getInstance().apply { timeInMillis = System.currentTimeMillis(); add(Calendar.MILLISECOND, delayMs) }
        setAlarm(ctx, cal.timeInMillis)
    }

    fun cancelAll(ctx: Context) {
        (ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager).cancel(pending(ctx))
        (ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancelAll()
    }

    /** Port of the original `TMGC_Util.ProcAlarmMes`: process the schedule file (on pause, and when an alarm fires). */
    fun process(ctx: Context) {
        val f = File(ctx.filesDir, INFO_FILE)
        val info = try { if (f.exists()) f.readBytes() else null } catch (e: Exception) { null } ?: return
        if (info.isEmpty() || info[0] == (-1).toByte()) return

        var passedAny = false
        var lastPassed: ByteArray? = null
        var cur = ByteArray(7)
        var idx = 0
        var hasFuture = false
        while (true) {
            val o = idx * 7
            if (o + 7 > info.size) break
            if (info[o] == (-1).toByte()) break
            cur = info.copyOfRange(o, o + 7)
            val cal = Calendar.getInstance().apply { clear(); set(cur[1] + 2000, cur[2] - 1, cur[3].toInt(), cur[4].toInt(), cur[5].toInt(), cur[6].toInt()) }
            if (System.currentTimeMillis() < cal.timeInMillis) { hasFuture = true; break }
            lastPassed = cur
            passedAny = true
            idx++
        }
        if (!passedAny) {
            if (hasFuture) scheduleAt(ctx, cur)
            return
        }
        if (lastPassed != null && lastPassed[0] != (-1).toByte()) notifyCall(ctx)
        val rest = info.size - idx * 7
        if (rest <= 0) ctx.deleteFile(INFO_FILE) else f.writeBytes(info.copyOfRange(idx * 7, info.size))
        if (hasFuture) scheduleAt(ctx, cur)
    }

    private fun scheduleAt(ctx: Context, r: ByteArray) {
        val cal = Calendar.getInstance().apply { clear(); set(r[1] + 2000, r[2] - 1, r[3].toInt(), r[4].toInt(), r[5].toInt(), r[6].toInt()); set(Calendar.MILLISECOND, 0) }
        setAlarm(ctx, cal.timeInMillis)
    }

    private fun setAlarm(ctx: Context, atMs: Long) {
        val am = ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager
        try {
            if (Build.VERSION.SDK_INT >= 31 && !am.canScheduleExactAlarms()) am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, atMs, pending(ctx))
            else am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, atMs, pending(ctx))
        } catch (e: SecurityException) {
            am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, atMs, pending(ctx))
        }
    }

    private fun pending(ctx: Context): PendingIntent =
        PendingIntent.getBroadcast(ctx, 0, Intent(ctx, AlarmReceiver::class.java), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun notifyCall(ctx: Context) {
        if (Build.VERSION.SDK_INT >= 33 && ctx.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            Log.i(TAG, "notification permission not granted; skipping"); return
        }
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val ch = NotificationChannel(CHANNEL, ctx.getString(R.string.call_channel), NotificationManager.IMPORTANCE_HIGH).apply {
            enableVibration(true); vibrationPattern = longArrayOf(0, 300, 500, 300)
        }
        nm.createNotificationChannel(ch)
        val open = PendingIntent.getActivity(ctx, 0, Intent(ctx, GameActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n = Notification.Builder(ctx, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_call)
            .setTicker(ctx.getString(R.string.call_ticker))
            .setContentTitle(ctx.getString(R.string.call_title))
            .setContentText(ctx.getString(R.string.call_text))
            .setContentIntent(open).setAutoCancel(true).build()
        nm.cancelAll()
        nm.notify(NOTIF_ID, n)
    }
}

class AlarmReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        try { AlarmLogic.process(context) } catch (e: Exception) { Log.e("AlarmReceiver", e.toString()) }
    }
}
