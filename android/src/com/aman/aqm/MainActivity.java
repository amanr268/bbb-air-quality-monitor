package com.aman.aqm;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * One-page live view of the BBB air-quality monitor. Subscribes to
 * aqm/bbb-aqi/# on the board's Mosquitto broker:
 *   sample (every second), avg (retained, every minute), status (retained
 *   online/offline), alarm, net (retained board network info, every 10 s).
 * Laid out for a ~400 dp wide portrait phone (Galaxy S25+).
 */
public class MainActivity extends Activity implements Mqtt.Listener {

    private static final String TOPIC = "aqm/bbb-aqi";
    private static final int BG = Color.rgb(16, 16, 18);
    private static final int CARD = Color.rgb(34, 34, 38);
    private static final int DIM = Color.rgb(160, 160, 168);
    private static final int OK = Color.rgb(80, 200, 120);
    private static final int BAD = Color.rgb(240, 80, 80);
    private static final int WARN = Color.rgb(240, 180, 60);

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Map<String, TextView> v = new HashMap<>();
    private Mqtt mqtt;
    private Thread mqttThread;

    /* last data, for ages */
    private long lastSampleMs, lastNetMs, lastWidgetMs;
    private String linkState = "-", linkHost = "-", linkIp = null, boardStatus = null;
    private long msgCount;

    private LinearLayout aqiBox;
    private TextView aqiNum, aqiCat, ageView;

    // ------------------------------------------------------------------ UI

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(BG);
        sv.setFillViewport(true);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(14);
        root.setPadding(pad, pad, pad, pad);
        sv.addView(root);
        sv.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View view, WindowInsets in) {
                int p = dp(14);
                root.setPadding(p, p + in.getSystemWindowInsetTop(), p,
                        p + in.getSystemWindowInsetBottom());
                return in;
            }
        });

        /* title row */
        LinearLayout title = row();
        TextView t = text("Air Quality Monitor", 22, Color.WHITE, true);
        title.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
        Button set = new Button(this);
        set.setText("Settings");
        set.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View x) { showSettings(); }
        });
        title.addView(set);
        root.addView(title);

        ageView = text("waiting for data…", 14, DIM, false);
        root.addView(ageView);

        /* AQI hero */
        aqiBox = new LinearLayout(this);
        aqiBox.setOrientation(LinearLayout.VERTICAL);
        aqiBox.setGravity(Gravity.CENTER_HORIZONTAL);
        aqiBox.setPadding(dp(12), dp(14), dp(12), dp(14));
        aqiBox.setBackground(round(Color.DKGRAY));
        aqiBox.addView(text("AQI (CPCB, PM-based)", 15, Color.BLACK, false));
        aqiNum = text("--", 76, Color.BLACK, true);
        aqiBox.addView(aqiNum);
        aqiCat = text("-", 24, Color.BLACK, true);
        aqiBox.addView(aqiCat);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(10);
        root.addView(aqiBox, lp);

        root.addView(section("Particles  (µg/m³)",
                tiles(new String[][] { { "pm1", "PM1.0" }, { "pm25", "PM2.5" }, { "pm10", "PM10" } })));
        root.addView(section("Gases",
                tiles(new String[][] { { "tvoc", "TVOC (ppb)" }, { "eco2", "eCO₂ (ppm)" } })));
        root.addView(section("Weather",
                tiles(new String[][] { { "temp", "Temperature" }, { "rh", "Humidity" } }),
                tiles(new String[][] { { "dew", "Dew point" }, { "feels", "Feels like" } })));

        root.addView(section("Averages", avgTable(), kv("aqi24", "24 h AQI")));

        root.addView(section("Sensors",
                kv("s_pms", "PMS5003  (particles)"),
                kv("s_ens", "ENS160  (TVOC / eCO₂)"),
                kv("s_aht", "AHT21  (temp / humidity)"),
                kv("alarm", "Last alarm")));

        TextView hItems = text("", 14, Color.WHITE, false);
        hItems.setPadding(0, dp(6), 0, 0);
        v.put("h_items", hItems);
        root.addView(section("Board health (daily self-test)",
                kv("h_res", "Result"),
                kv("h_when", "Last run"),
                hItems));

        root.addView(section("Connection",
                kv("c_phone", "Phone network"),
                kv("c_path", "Phone → board"),
                kv("c_mqtt", "MQTT link"),
                kv("c_board", "Board"),
                kv("c_msgs", "Messages received")));

        root.addView(section("Board Wi-Fi",
                kv("w_ssid", "Network (SSID)"),
                kv("w_sig", "Signal"),
                kv("w_band", "Band / channel"),
                kv("w_rate", "Link rate"),
                kv("w_ip", "Board IP (home)"),
                kv("w_gw", "Router"),
                kv("w_ts", "Board IP (Tailscale)"),
                kv("w_up", "Board uptime"),
                kv("w_load", "CPU load / free RAM")));

        setContentView(sv);

        ui.post(tick);
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (prefs().getString("pass", "").isEmpty())
            showSettings();
        else
            startMqtt();
    }

    @Override
    protected void onStop() {
        super.onStop();
        stopMqtt();
    }

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            updateAge();
            set("c_phone", phoneNet());
            ui.postDelayed(this, 1000);
        }
    };

    // ---------------------------------------------------------- MQTT glue

    private SharedPreferences prefs() {
        return getSharedPreferences("aqm", MODE_PRIVATE);
    }

    private void startMqtt() {
        stopMqtt();
        SharedPreferences p = prefs();
        String hosts = p.getString("hosts", "192.168.1.14,bbb-aqi");
        int port;
        try { port = Integer.parseInt(p.getString("port", "1883").trim()); }
        catch (NumberFormatException e) { port = 1883; }
        mqtt = new Mqtt(hosts.split(","), port, p.getString("user", "phone"),
                p.getString("pass", ""), TOPIC + "/#", this);
        mqttThread = new Thread(mqtt, "mqtt");
        mqttThread.start();
    }

    private void stopMqtt() {
        if (mqtt != null) mqtt.stop();
        mqtt = null;
        mqttThread = null;
    }

    @Override
    public void onState(final String state, final String host, final String ip) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                linkState = state;
                linkHost = host;
                linkIp = ip;
                showLink();
            }
        });
    }

    @Override
    public void onMessage(final String topic, final String payload) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                msgCount++;
                set("c_msgs", String.valueOf(msgCount));
                String sub = topic.substring(topic.lastIndexOf('/') + 1);
                try {
                    switch (sub) {
                        case "sample": onSample(new JSONObject(payload)); break;
                        case "avg":    onAvg(new JSONObject(payload)); break;
                        case "net":    onNet(new JSONObject(payload)); break;
                        case "alarm":  onAlarm(new JSONObject(payload)); break;
                        case "health": onHealth(new JSONObject(payload)); break;
                        case "status": boardStatus = payload.trim(); showLink(); break;
                        default: break;
                    }
                } catch (Exception e) {
                    /* malformed message: ignore */
                }
            }
        });
    }

    private void showLink() {
        boolean up = "connected".equals(linkState);
        set("c_mqtt", up ? "connected" : linkState, up ? OK : (linkState.startsWith("error") ? BAD : WARN));
        String path;
        if (!up) path = "trying " + linkHost;
        else if (linkIp != null && isTailscale(linkIp)) path = "Tailscale  (" + linkHost + ", " + linkIp + ")";
        else path = "Home Wi-Fi  (" + linkIp + ")";
        set("c_path", path);
        if (boardStatus == null) set("c_board", "-");
        else set("c_board", boardStatus, "online".equals(boardStatus) ? OK : BAD);
    }

    private static boolean isTailscale(String ip) {
        /* 100.64.0.0/10 */
        String[] p = ip.split("\\.");
        if (p.length != 4) return ip.startsWith("fd7a:115c:a1e0");
        try {
            int a = Integer.parseInt(p[0]), b = Integer.parseInt(p[1]);
            return a == 100 && b >= 64 && b <= 127;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    private String phoneNet() {
        ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return "-";
        Network n = cm.getActiveNetwork();
        NetworkCapabilities c = n != null ? cm.getNetworkCapabilities(n) : null;
        if (c == null) return "offline";
        String s = c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ? "Wi-Fi"
                : c.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) ? "Mobile data"
                : c.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) ? "Ethernet" : "other";
        boolean vpn = false;
        for (Network x : cm.getAllNetworks()) {
            NetworkCapabilities xc = cm.getNetworkCapabilities(x);
            if (xc != null && xc.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) vpn = true;
        }
        return vpn ? s + " + VPN (Tailscale)" : s;
    }

    // -------------------------------------------------------------- data

    private void onSample(JSONObject j) {
        lastSampleMs = System.currentTimeMillis();
        int st = j.optInt("status", 0);
        boolean pmOk = (st & 1) == 0, ensOk = (st & 2) == 0, ahtOk = (st & 4) == 0;

        set("pm1", num(j, "pm1", 0));
        set("pm25", num(j, "pm25", 1));
        set("pm10", num(j, "pm10", 1));
        set("tvoc", num(j, "tvoc", 0));
        set("eco2", num(j, "eco2", 0));
        set("temp", j.isNull("temp") ? "--" : num(j, "temp", 1) + " °C");
        set("rh", j.isNull("rh") ? "--" : num(j, "rh", 1) + " %");

        if (!j.isNull("temp") && !j.isNull("rh")) {
            double t = j.optDouble("temp"), rh = j.optDouble("rh");
            set("dew", String.format(Locale.US, "%.1f °C", dewPoint(t, rh)));
            set("feels", String.format(Locale.US, "%.1f °C", apparent(t, rh)));
        } else {
            set("dew", "--");
            set("feels", "--");
        }

        if (j.isNull("aqi")) {
            aqiNum.setText("--");
            aqiCat.setText("PM sensor fault");
            aqiBox.setBackground(round(Color.DKGRAY));
            setAqiText(Color.WHITE);
        } else {
            int aqi = j.optInt("aqi");
            aqiNum.setText(String.valueOf(aqi));
            aqiCat.setText(j.optString("cat", "-"));
            int c = aqiColor(aqi);
            aqiBox.setBackground(round(c));
            setAqiText(aqi > 300 ? Color.WHITE : Color.BLACK);
        }

        set("s_pms", pmOk ? "OK" : "FAULT", pmOk ? OK : BAD);
        set("s_ens", ensOk ? "OK" : "FAULT", ensOk ? OK : BAD);
        set("s_aht", ahtOk ? "OK" : "FAULT", ahtOk ? OK : BAD);
        updateAge();
        if (lastSampleMs - lastWidgetMs >= 10000) {   /* keep the widget fresh while the app is open */
            lastWidgetMs = lastSampleMs;
            AqmWidget.saveSample(this, j.toString(), AqmWidget.pathName(linkIp));
            AqmWidget.render(this);
        }
    }

    private void onAvg(JSONObject j) {
        String[] m = { "pm25", "pm10", "tvoc", "temp", "rh" };
        String[] w = { "1m", "5m", "10m", "20m", "24h" };
        for (String k : m) {
            JSONObject o = j.optJSONObject(k);
            for (String x : w)
                set("a_" + k + "_" + x, o == null ? "--" : num(o, x, k.equals("tvoc") ? 0 : 1));
        }
        int mins = j.optInt("minutes24h", 0);
        if (j.isNull("aqi24h"))
            set("aqi24", "building: " + (mins / 60) + " / 16 h of data", DIM);
        else
            set("aqi24", j.optInt("aqi24h") + "  (" + (mins / 60) + " h of data)", aqiColor(j.optInt("aqi24h")));
    }

    private void onNet(JSONObject j) {
        lastNetMs = System.currentTimeMillis();
        set("w_ssid", j.optString("ssid", "-").isEmpty() ? "not connected" : j.optString("ssid"));
        if (j.isNull("signal")) {
            set("w_sig", "-");
        } else {
            int dbm = j.optInt("signal");
            String q = dbm >= -55 ? "excellent" : dbm >= -67 ? "good" : dbm >= -75 ? "fair" : "weak";
            set("w_sig", dbm + " dBm  (" + q + ")", dbm >= -67 ? OK : dbm >= -75 ? WARN : BAD);
        }
        if (j.isNull("freq")) {
            set("w_band", "-");
        } else {
            int f = j.optInt("freq");
            String band = f < 3000 ? "2.4 GHz" : f < 5925 ? "5 GHz" : "6 GHz";
            int ch = f < 3000 ? (f == 2484 ? 14 : (f - 2407) / 5) : (f - 5000) / 5;
            set("w_band", band + ", channel " + ch + "  (" + f + " MHz)");
        }
        set("w_rate", j.isNull("rate") ? "-" : j.optString("rate") + " Mbit/s");
        set("w_ip", j.optString("ip", "-"));
        set("w_gw", j.optString("gw", "-"));
        String ts = j.optString("ts_ip", "");
        set("w_ts", ts.isEmpty() ? "not running" : ts);
        long up = j.optLong("uptime", 0);
        set("w_up", String.format(Locale.US, "%dd %02dh %02dm", up / 86400, (up / 3600) % 24, (up / 60) % 60));
        set("w_load", j.optString("load", "-") + "  /  " + (j.optLong("mem_avail_kb", 0) / 1024) + " MB");
    }

    /* summary of scripts/aqm_selftest.sh, retained on aqm/bbb-aqi/health */
    private void onHealth(JSONObject j) {
        int pass = j.optInt("pass"), warn = j.optInt("warn"), fail = j.optInt("fail");
        long ts = j.optLong("ts") * 1000;
        long ageH = (System.currentTimeMillis() - ts) / 3600000L;
        String res = fail > 0 ? fail + " FAIL, " + warn + " WARN" : warn > 0 ? "OK, " + warn + " WARN" : "All OK";
        set("h_res", res + "  (" + pass + " passed)", fail > 0 ? BAD : warn > 0 ? WARN : OK);
        set("h_when", new SimpleDateFormat("d MMM, h:mm a", Locale.US).format(new Date(ts))
                + (ageH >= 26 ? "  (overdue)" : ""), ageH >= 26 ? WARN : Color.WHITE);
        StringBuilder sb = new StringBuilder();
        JSONArray items = j.optJSONArray("items");
        for (int i = 0; items != null && i < items.length(); i++) {
            JSONObject it = items.optJSONObject(i);
            if (it == null) continue;
            if (sb.length() > 0) sb.append("\n\n");
            sb.append(it.optString("s")).append(": ").append(it.optString("t"));
        }
        set("h_items", sb.length() > 0 ? sb.toString() : "No warnings or failures.",
                fail > 0 ? BAD : warn > 0 ? WARN : DIM);
    }

    private void onAlarm(JSONObject j) {
        String when = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date(j.optLong("ts") * 1000));
        set("alarm", when + "  " + j.optString("text"), j.optInt("prio") >= 2 ? BAD : WARN);
    }

    private void updateAge() {
        if (lastSampleMs == 0) {
            ageView.setText("waiting for data…");
            ageView.setTextColor(DIM);
            return;
        }
        long s = (System.currentTimeMillis() - lastSampleMs) / 1000;
        ageView.setText("Live · last reading " + (s == 0 ? "just now" : s + " s ago"));
        ageView.setTextColor(s <= 5 ? OK : s <= 30 ? WARN : BAD);
    }

    // ---------------------------------------------------------- weather

    /** Magnus formula */
    private static double dewPoint(double t, double rh) {
        double a = 17.62, b = 243.12;
        double g = Math.log(Math.max(rh, 1) / 100.0) + a * t / (b + t);
        return b * g / (a - g);
    }

    /** Australian apparent temperature, no wind (shade, indoor-ish) */
    private static double apparent(double t, double rh) {
        double e = rh / 100.0 * 6.105 * Math.exp(17.27 * t / (237.7 + t));
        return t + 0.33 * e - 4.0;
    }

    /** CPCB category colours */
    private static int aqiColor(int aqi) {
        if (aqi <= 50) return Color.rgb(0, 176, 80);
        if (aqi <= 100) return Color.rgb(146, 208, 80);
        if (aqi <= 200) return Color.rgb(255, 230, 0);
        if (aqi <= 300) return Color.rgb(255, 153, 0);
        if (aqi <= 400) return Color.rgb(255, 0, 0);
        return Color.rgb(192, 0, 0);
    }

    private void setAqiText(int c) {
        for (int i = 0; i < aqiBox.getChildCount(); i++)
            ((TextView) aqiBox.getChildAt(i)).setTextColor(c);
    }

    // ---------------------------------------------------------- settings

    private void showSettings() {
        final SharedPreferences p = prefs();
        LinearLayout f = new LinearLayout(this);
        f.setOrientation(LinearLayout.VERTICAL);
        f.setPadding(dp(20), dp(8), dp(20), 0);
        final EditText hosts = field(f, "Board addresses, tried in order (home IP, Tailscale name)",
                p.getString("hosts", "192.168.1.14,bbb-aqi"), InputType.TYPE_CLASS_TEXT);
        final EditText port = field(f, "MQTT port", p.getString("port", "1883"), InputType.TYPE_CLASS_NUMBER);
        final EditText user = field(f, "Username", p.getString("user", "phone"), InputType.TYPE_CLASS_TEXT);
        final EditText pass = field(f, "Password", p.getString("pass", ""),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        new AlertDialog.Builder(this)
                .setTitle("Connection settings")
                .setView(f)
                .setPositiveButton("Save", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        p.edit().putString("hosts", hosts.getText().toString().trim())
                                .putString("port", port.getText().toString().trim())
                                .putString("user", user.getText().toString().trim())
                                .putString("pass", pass.getText().toString())
                                .apply();
                        startMqtt();
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private EditText field(LinearLayout f, String label, String val, int type) {
        f.addView(text(label, 13, DIM, false));
        EditText e = new EditText(this);
        e.setText(val);
        e.setInputType(type);
        e.setSingleLine(true);
        f.addView(e);
        return e;
    }

    // ---------------------------------------------------------- builders

    private int dp(int x) {
        return Math.round(x * getResources().getDisplayMetrics().density);
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        return r;
    }

    private GradientDrawable round(int color) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(14));
        return g;
    }

    private View section(String title, View... content) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(round(CARD));
        c.setPadding(dp(14), dp(10), dp(14), dp(12));
        c.addView(text(title, 15, DIM, true));
        for (View x : content) c.addView(x);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(12);
        c.setLayoutParams(lp);
        return c;
    }

    /** row of equal tiles: big value, small label */
    private View tiles(String[][] items) {
        LinearLayout r = row();
        r.setPadding(0, dp(6), 0, 0);
        for (String[] it : items) {
            LinearLayout cell = new LinearLayout(this);
            cell.setOrientation(LinearLayout.VERTICAL);
            TextView val = text("--", 30, Color.WHITE, true);
            v.put(it[0], val);
            cell.addView(val);
            cell.addView(text(it[1], 13, DIM, false));
            r.addView(cell, new LinearLayout.LayoutParams(0, -2, 1));
        }
        return r;
    }

    /** label left, value right */
    private View kv(String key, String label) {
        LinearLayout r = row();
        r.setPadding(0, dp(7), 0, 0);
        TextView l = text(label, 15, DIM, false);
        r.addView(l, new LinearLayout.LayoutParams(0, -2, 1));
        TextView val = text("-", 16, Color.WHITE, true);
        val.setGravity(Gravity.END);
        v.put(key, val);
        r.addView(val, new LinearLayout.LayoutParams(0, -2, 1.3f));
        return r;
    }

    private View avgTable() {
        LinearLayout t = new LinearLayout(this);
        t.setOrientation(LinearLayout.VERTICAL);
        String[] w = { "1m", "5m", "10m", "20m", "24h" };
        String[][] m = { { "pm25", "PM2.5" }, { "pm10", "PM10" }, { "tvoc", "TVOC" },
                         { "temp", "Temp °C" }, { "rh", "RH %" } };
        LinearLayout h = row();
        h.setPadding(0, dp(6), 0, 0);
        h.addView(text("", 13, DIM, false), new LinearLayout.LayoutParams(0, -2, 1.3f));
        for (String x : w) {
            TextView c = text(x, 13, DIM, true);
            c.setGravity(Gravity.END);
            h.addView(c, new LinearLayout.LayoutParams(0, -2, 1));
        }
        t.addView(h);
        for (String[] mm : m) {
            LinearLayout r = row();
            r.setPadding(0, dp(5), 0, 0);
            r.addView(text(mm[1], 14, DIM, false), new LinearLayout.LayoutParams(0, -2, 1.3f));
            for (String x : w) {
                TextView c = text("--", 15, Color.WHITE, false);
                c.setGravity(Gravity.END);
                v.put("a_" + mm[0] + "_" + x, c);
                r.addView(c, new LinearLayout.LayoutParams(0, -2, 1));
            }
            t.addView(r);
        }
        return t;
    }

    private void set(String key, String s) {
        set(key, s, Color.WHITE);
    }

    private void set(String key, String s, int color) {
        TextView t = v.get(key);
        if (t == null) return;
        t.setText(s);
        t.setTextColor(color);
    }

    private static String num(JSONObject j, String k, int dec) {
        if (j.isNull(k)) return "--";
        return String.format(Locale.US, dec == 0 ? "%.0f" : "%.1f", j.optDouble(k));
    }
}
