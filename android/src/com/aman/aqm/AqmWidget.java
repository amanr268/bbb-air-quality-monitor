package com.aman.aqm;

import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.widget.RemoteViews;

import org.json.JSONObject;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Home-screen widget: AQI tile in the CPCB colour, PM2.5/PM10, temperature,
 * humidity, TVOC/eCO2 and when it was updated. A widget cannot hold a live
 * connection, so it is refreshed by FetchJob every 15 min (Android's minimum),
 * on tapping ⟳, and every 10 s while the app itself is open.
 */
public class AqmWidget extends AppWidgetProvider {

    static final String ACTION_REFRESH = "com.aman.aqm.REFRESH";
    private static final int JOB_PERIODIC = 1, JOB_NOW = 2;

    @Override
    public void onUpdate(Context c, AppWidgetManager m, int[] ids) {
        render(c);
        schedule(c);
    }

    @Override
    public void onEnabled(Context c) {
        schedule(c);
        refreshNow(c);
    }

    @Override
    public void onDisabled(Context c) {
        JobScheduler js = (JobScheduler) c.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (js != null) {
            js.cancel(JOB_PERIODIC);
            js.cancel(JOB_NOW);
        }
    }

    @Override
    public void onReceive(Context c, Intent i) {
        super.onReceive(c, i);
        if (ACTION_REFRESH.equals(i.getAction())) {
            store(c).edit().putBoolean("busy", true).apply();
            render(c);
            refreshNow(c);
        }
    }

    // ---------------------------------------------------------- jobs

    static void schedule(Context c) {
        JobScheduler js = (JobScheduler) c.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (js == null) return;
        for (JobInfo j : js.getAllPendingJobs())
            if (j.getId() == JOB_PERIODIC) return;
        js.schedule(new JobInfo.Builder(JOB_PERIODIC, new ComponentName(c, FetchJob.class))
                .setPeriodic(15 * 60 * 1000L)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setPersisted(true)
                .build());
    }

    static void refreshNow(Context c) {
        JobScheduler js = (JobScheduler) c.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (js == null) return;
        js.schedule(new JobInfo.Builder(JOB_NOW, new ComponentName(c, FetchJob.class))
                .setOverrideDeadline(0)
                .build());
    }

    // ---------------------------------------------------------- data

    static SharedPreferences store(Context c) {
        return c.getSharedPreferences("widget", Context.MODE_PRIVATE);
    }

    /** remember a fresh sample (from the app or from FetchJob) */
    static void saveSample(Context c, String sampleJson, String path) {
        store(c).edit()
                .putString("sample", sampleJson)
                .putLong("ok_ms", System.currentTimeMillis())
                .putString("path", path)
                .putBoolean("failed", false)
                .putBoolean("busy", false)
                .apply();
    }

    static void saveFailure(Context c, String why) {
        store(c).edit().putBoolean("failed", true).putBoolean("busy", false)
                .putString("why", why).apply();
    }

    // ---------------------------------------------------------- drawing

    static void render(Context c) {
        AppWidgetManager m = AppWidgetManager.getInstance(c);
        ComponentName cn = new ComponentName(c, AqmWidget.class);
        if (m.getAppWidgetIds(cn).length == 0) return;

        RemoteViews rv = new RemoteViews(c.getPackageName(), R.layout.widget);
        SharedPreferences p = store(c);
        JSONObject s = null;
        try {
            String js = p.getString("sample", null);
            if (js != null) s = new JSONObject(js);
        } catch (Exception ignored) { }

        int tile = Color.DKGRAY, ink = Color.WHITE;
        if (s != null && !s.isNull("aqi")) {
            int aqi = s.optInt("aqi");
            tile = aqiColor(aqi);
            ink = aqi > 300 ? Color.WHITE : Color.BLACK;
            rv.setTextViewText(R.id.aqi, String.valueOf(aqi));
            rv.setTextViewText(R.id.cat, s.optString("cat", "-"));
        } else {
            rv.setTextViewText(R.id.aqi, "--");
            rv.setTextViewText(R.id.cat, s == null ? "no data" : "PM fault");
        }
        rv.setInt(R.id.aqi_bg, "setColorFilter", tile);
        rv.setTextColor(R.id.aqi_lbl, ink);
        rv.setTextColor(R.id.aqi, ink);
        rv.setTextColor(R.id.cat, ink);

        if (s != null) {
            rv.setTextViewText(R.id.l1, "PM2.5 " + n(s, "pm25", 0) + "   PM10 " + n(s, "pm10", 0));
            rv.setTextViewText(R.id.l2, n(s, "temp", 1) + " °C   " + n(s, "rh", 0) + " %RH");
            rv.setTextViewText(R.id.l3, "TVOC " + n(s, "tvoc", 0) + "   eCO₂ " + n(s, "eco2", 0));
        }

        String t = new SimpleDateFormat("HH:mm", Locale.US).format(new Date(p.getLong("ok_ms", 0)));
        String upd;
        int updColor = Color.rgb(160, 160, 168);
        if (p.getBoolean("busy", false)) {
            upd = "updating…";
        } else if (s == null) {
            upd = p.getBoolean("failed", false) ? "board not reachable" : "tap ⟳ to load";
        } else if (p.getBoolean("failed", false)) {
            upd = "offline · last " + t;
            updColor = Color.rgb(240, 180, 60);
        } else {
            upd = t + " · " + p.getString("path", "");
        }
        rv.setTextViewText(R.id.upd, upd);
        rv.setTextColor(R.id.upd, updColor);

        Intent open = new Intent(c, MainActivity.class);
        rv.setOnClickPendingIntent(R.id.root, PendingIntent.getActivity(c, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        Intent ref = new Intent(c, AqmWidget.class).setAction(ACTION_REFRESH);
        rv.setOnClickPendingIntent(R.id.refresh, PendingIntent.getBroadcast(c, 1, ref,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));

        m.updateAppWidget(cn, rv);
    }

    private static String n(JSONObject j, String k, int dec) {
        if (j.isNull(k)) return "--";
        return String.format(Locale.US, dec == 0 ? "%.0f" : "%.1f", j.optDouble(k));
    }

    static int aqiColor(int aqi) {
        if (aqi <= 50) return Color.rgb(0, 176, 80);
        if (aqi <= 100) return Color.rgb(146, 208, 80);
        if (aqi <= 200) return Color.rgb(255, 230, 0);
        if (aqi <= 300) return Color.rgb(255, 153, 0);
        if (aqi <= 400) return Color.rgb(255, 0, 0);
        return Color.rgb(192, 0, 0);
    }

    static String pathName(String remoteIp) {
        if (remoteIp == null) return "";
        String[] q = remoteIp.split("\\.");
        try {
            if (q.length == 4 && Integer.parseInt(q[0]) == 100) {
                int b = Integer.parseInt(q[1]);
                if (b >= 64 && b <= 127) return "Tailscale";
            }
        } catch (NumberFormatException ignored) { }
        return "Home Wi-Fi";
    }
}
