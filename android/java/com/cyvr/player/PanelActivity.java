package com.cyvr.player;
/** Default Home/Overlay activity: a system-managed 2D window, no OpenXR session. */
public final class PanelActivity extends PlayerActivity {
    @Override public int presentationMode(){return 0;}
}
