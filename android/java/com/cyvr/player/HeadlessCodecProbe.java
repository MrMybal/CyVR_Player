package com.cyvr.player;
import android.media.MediaFormat;
/** Developer command for app_process; does not create a UI or load native code. */
public final class HeadlessCodecProbe {
    public static void main(String[] args) {
        if (args.length!=4) throw new IllegalArgumentException("MIME width height frameRate");
        MediaFormat format=MediaFormat.createVideoFormat(args[0],Integer.parseInt(args[1]),Integer.parseInt(args[2]));
        format.setFloat(MediaFormat.KEY_FRAME_RATE,Float.parseFloat(args[3]));
        for (String name:CodecProbe.hardwareCandidates(format)) System.out.println(name);
    }
}
