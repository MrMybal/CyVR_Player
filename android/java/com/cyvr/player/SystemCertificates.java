package com.cyvr.player;

import android.content.Context;
import android.util.AtomicFile;
import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.KeyStore;
import java.security.cert.X509Certificate;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/** Export platform trust anchors for FFmpeg/Mbed TLS. Run on a worker at
 * startup so trust-store updates are reflected. Certificate verification
 * remains enabled; no global trust-store or Android setting is changed. */
public final class SystemCertificates {
    private SystemCertificates() {}
    public static String exportPem(Context context) throws GeneralSecurityException, IOException {
        TrustManagerFactory factory=TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((KeyStore)null);
        ByteArrayOutputStream pem=new ByteArrayOutputStream();
        int count=0;
        for (TrustManager manager:factory.getTrustManagers()) {
            if (!(manager instanceof X509TrustManager)) continue;
            for (X509Certificate certificate:((X509TrustManager)manager).getAcceptedIssuers()) {
                pem.write("-----BEGIN CERTIFICATE-----\n".getBytes(StandardCharsets.US_ASCII));
                pem.write(Base64.encode(certificate.getEncoded(),Base64.NO_WRAP));
                pem.write("\n-----END CERTIFICATE-----\n".getBytes(StandardCharsets.US_ASCII));
                ++count;
            }
        }
        if (count==0) throw new GeneralSecurityException("Android returned no trusted certificates");
        File file=new File(context.getNoBackupFilesDir(),"cyvr-system-ca.pem");
        AtomicFile atomic=new AtomicFile(file);
        FileOutputStream output=null;
        try {
            output=atomic.startWrite(); output.write(pem.toByteArray()); atomic.finishWrite(output);
        } catch (IOException error) {
            if (output!=null) atomic.failWrite(output);
            throw error;
        }
        return file.getAbsolutePath();
    }
}
