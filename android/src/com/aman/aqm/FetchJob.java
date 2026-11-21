package com.aman.aqm;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.content.Context;
import android.content.SharedPreferences;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Widget refresh: connect to the board's broker with the app's settings,
 * wait for one live sample (max 12 s), save it, redraw the widget, disconnect.
 */
public class FetchJob extends JobService {

    @Override
    public boolean onStartJob(final JobParameters params) {
        final Context c = getApplicationContext();
        new Thread(new Runnable() {
            @Override
            public void run() {
                fetch(c);
                AqmWidget.render(c);
                jobFinished(params, false);
            }
        }, "widget-fetch").start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return false;
    }

    static void fetch(final Context c) {
        SharedPreferences p = c.getSharedPreferences("aqm", Context.MODE_PRIVATE);
        String pass = p.getString("pass", "");
        if (pass.isEmpty()) {
            AqmWidget.saveFailure(c, "open the app and set the password");
            return;
        }
        int port;
        try { port = Integer.parseInt(p.getString("port", "1883").trim()); }
        catch (NumberFormatException e) { port = 1883; }

        final CountDownLatch got = new CountDownLatch(1);
        final String[] ip = new String[1];
        final String[] err = { "board not reachable" };
        Mqtt m = new Mqtt(p.getString("hosts", "192.168.1.14,bbb-aqi").split(","), port,
                p.getString("user", "phone"), pass, "aqm/bbb-aqi/sample",
                new Mqtt.Listener() {
                    @Override
                    public void onState(String state, String host, String remoteIp) {
                        if ("connected".equals(state)) ip[0] = remoteIp;
                        else if (state.startsWith("error")) err[0] = state;
                    }

                    @Override
                    public void onMessage(String topic, String payload) {
                        if (got.getCount() > 0) {
                            AqmWidget.saveSample(c, payload, AqmWidget.pathName(ip[0]));
                            got.countDown();
                        }
                    }
                });
        Thread t = new Thread(m, "widget-mqtt");
        t.start();
        boolean ok = false;
        try {
            ok = got.await(12, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) { }
        m.stop();
        try { t.join(2000); } catch (InterruptedException ignored) { }
        if (!ok) AqmWidget.saveFailure(c, err[0]);
    }
}
