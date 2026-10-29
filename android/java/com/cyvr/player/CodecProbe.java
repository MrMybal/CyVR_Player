package com.cyvr.player;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import java.util.ArrayList;

/** Android API 29+. Call off the render thread, using the full stream format.
 * Capability reports are candidates: configure and sustained playback must
 * still succeed. This class has no UI and is not yet connected through JNI. */
public final class CodecProbe {
    private CodecProbe() {}

    public static String[] hardwareCandidates(MediaFormat format) {
        String mime = format.getString(MediaFormat.KEY_MIME);
        if (mime == null || !mime.startsWith("video/"))
            throw new IllegalArgumentException("A video MIME type is required");
        ArrayList<String> names = new ArrayList<>();
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !info.isHardwareAccelerated() || info.isAlias()) continue;
            try {
                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(mime);
                if (caps.isFeatureRequired(MediaCodecInfo.CodecCapabilities.FEATURE_SecurePlayback)
                        || caps.isFeatureRequired(MediaCodecInfo.CodecCapabilities.FEATURE_TunneledPlayback)
                        || !caps.isFormatSupported(format)) continue;
                MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                if (video == null) continue;
                int width = format.getInteger(MediaFormat.KEY_WIDTH);
                int height = format.getInteger(MediaFormat.KEY_HEIGHT);
                if (!video.isSizeSupported(width, height)) continue;
                if (format.containsKey(MediaFormat.KEY_FRAME_RATE)) {
                    Number rate = format.getNumber(MediaFormat.KEY_FRAME_RATE);
                    if (rate != null && rate.doubleValue() > 0
                            && !video.areSizeAndRateSupported(width, height, rate.doubleValue())) continue;
                }
                names.add(info.getName());
            } catch (IllegalArgumentException ignored) {
                // Codec does not support this MIME, profile or stream format.
            }
        }
        return names.toArray(new String[0]);
    }
}
