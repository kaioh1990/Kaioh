package org.libsdl.app;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * CvF Native: HTTP for the native engine (the lobby, the public-IP check, UPnP).
 * Android has no libcurl; the engine's net/http calls this from its own worker
 * threads through JNI (engine/src/platform/android/android_http.c), one request
 * per call, blocking. System TLS, certificates and proxy settings apply.
 */
public class CvfHttp {
    private CvfHttp() {}

    /**
     * @param headers   "Key: Value" lines separated by '\n'
     * @param body      request body or null
     * @param timeoutMs connect and read timeout, 0 = none (Go's Client.Timeout)
     * @param status    status[0] gets the HTTP status code
     * @param error     error[0] gets a message if the request failed (then null is returned)
     * @return the response body (also for error statuses, as Go returns it)
     */
    public static byte[] request(String method, String url, String headers, byte[] body, int timeoutMs,
                                 int[] status, String[] error) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod(method);
            c.setInstanceFollowRedirects(true);
            c.setUseCaches(false);
            c.setConnectTimeout(Math.max(timeoutMs, 0));
            c.setReadTimeout(Math.max(timeoutMs, 0));
            c.setRequestProperty("User-Agent", "Go-http-client/1.1");
            for (String line : headers.split("\n")) {
                int i = line.indexOf(':');
                if (i > 0) {
                    c.setRequestProperty(line.substring(0, i).trim(), line.substring(i + 1).trim());
                }
            }
            if (body != null && body.length > 0) {
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                try (OutputStream o = c.getOutputStream()) {
                    o.write(body);
                }
            }
            int st = c.getResponseCode();
            status[0] = st;
            InputStream in = st >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            if (in != null) {
                try {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                } finally {
                    in.close();
                }
            }
            return out.toByteArray();
        } catch (Throwable e) {
            error[0] = e.toString();
            return null;
        } finally {
            if (c != null) {
                c.disconnect();
            }
        }
    }
}
