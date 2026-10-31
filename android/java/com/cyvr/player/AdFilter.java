package com.cyvr.player;

import android.net.Uri;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/** Blocks requests to well-known advertising and tracking hosts, by domain. */
final class AdFilter {
    private static final Set<String> HOSTS = new HashSet<>(Arrays.asList(
        "2mdn.net", "33across.com", "3lift.com", "adcolony.com", "adform.net", "adition.com",
        "adnxs.com", "adroll.com", "adsafeprotected.com", "adsrvr.org", "adservice.google.com",
        "advertising.com", "amazon-adsystem.com", "applovin.com", "appnexus.com", "bidswitch.net",
        "bluekai.com", "casalemedia.com", "chartbeat.com", "chartbeat.net", "contextweb.com",
        "criteo.com", "criteo.net", "demdex.net", "doubleclick.net", "doubleverify.com",
        "exoclick.com", "exosrv.com", "flashtalking.com", "fwmrm.net", "google-analytics.com",
        "googleadservices.com", "googlesyndication.com", "googletagmanager.com",
        "googletagservices.com", "gumgum.com", "hotjar.com", "ib-ibi.com", "imrworldwide.com",
        "indexww.com", "inmobi.com", "juicyads.com", "krxd.net", "liadm.com", "media.net",
        "moatads.com", "mookie1.com", "omtrdc.net", "openx.net", "outbrain.com", "popads.net",
        "popcash.net", "propellerads.com", "pubmatic.com", "quantserve.com", "rlcdn.com",
        "rubiconproject.com", "scorecardresearch.com", "sharethrough.com", "smartadserver.com",
        "spotxchange.com", "taboola.com", "teads.tv", "tradedoubler.com", "trafficjunky.net",
        "tribalfusion.com", "yieldmo.com", "zedo.com"));

    private AdFilter() {}

    static boolean blocks(Uri url) {
        String host = url.getHost();
        if (host == null)
            return false;
        host = host.toLowerCase(Locale.ROOT);
        for (int dot = 0; dot >= 0; dot = host.indexOf('.', dot) + 1) {
            if (HOSTS.contains(host.substring(dot)))
                return true;
            if (host.indexOf('.', dot) < 0)
                return false;
        }
        return false;
    }
}
