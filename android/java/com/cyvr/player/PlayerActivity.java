package com.cyvr.player;

import android.app.NativeActivity;
import android.app.PendingIntent;
import android.content.Intent;
import android.media.MediaFormat;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import java.util.ArrayDeque;
import java.util.ArrayList;

/** Android system access only. The player UI is entirely C++ / CyVR_UI. */
public class PlayerActivity extends NativeActivity {
    private static java.lang.ref.WeakReference<PlayerActivity> active=new java.lang.ref.WeakReference<>(null);
    private final ArrayDeque<String[]> events=new ArrayDeque<>();
    private CancellationSignal query;
    private final java.util.concurrent.ExecutorService cancellationWorker=java.util.concurrent.Executors.newSingleThreadExecutor();
    private final java.util.concurrent.ScheduledExecutorService thumbnailTimer=java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
    private final ArrayList<DocumentBrowser.OpenDocument> openDocuments=new ArrayList<>();
    @Override public void onCreate(Bundle state) {
        PlayerActivity previous=active.get();
        if(previous!=null && previous!=this && previous.presentationMode()!=presentationMode() && !previous.isFinishing())
            previous.finishAndRemoveTask();
        active=new java.lang.ref.WeakReference<>(this);
        acceptIntent(getIntent());super.onCreate(state);
    }
    public int presentationMode(){return 0;}
    public int initialPage(){return getIntent().getIntExtra("cyvr.page",0);}
    public void launchPresentation(final int mode,final int page){runOnUiThread(()->{
        if(mode==1){
            Intent immersive=new Intent(this,ImmersiveActivity.class).setAction(Intent.ACTION_MAIN)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).putExtra("cyvr.page",page);
            startActivity(immersive);finishAndRemoveTask();
        }else{
            Intent panel=new Intent(this,PanelActivity.class).setAction(Intent.ACTION_MAIN)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).putExtra("cyvr.page",page);
            PendingIntent pending=PendingIntent.getActivity(this,0,panel,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_IMMUTABLE);
            Intent home=new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra("extra_launch_in_home_pending_intent",pending);
            startActivity(home);finishAndRemoveTask();
        }
    });}
    @Override public void onNewIntent(Intent intent) {super.onNewIntent(intent);setIntent(intent);acceptIntent(intent);}
    @Override public boolean dispatchKeyEvent(android.view.KeyEvent event){
        if(event.getKeyCode()==android.view.KeyEvent.KEYCODE_BACK){
            if(event.getAction()==android.view.KeyEvent.ACTION_UP)enqueue("back","","");
            return true;
        }
        int code=event.getKeyCode();
        if(code==android.view.KeyEvent.KEYCODE_DPAD_LEFT || code==android.view.KeyEvent.KEYCODE_DPAD_RIGHT
                || code==android.view.KeyEvent.KEYCODE_DPAD_UP || code==android.view.KeyEvent.KEYCODE_DPAD_DOWN){
            if(event.getAction()==android.view.KeyEvent.ACTION_DOWN && event.getRepeatCount()==0)
                enqueue("video_key",Integer.toString(code),"");
            return true;
        }
        if(code==android.view.KeyEvent.KEYCODE_F11){
            if(event.getAction()==android.view.KeyEvent.ACTION_UP)enqueue("toggle_interface","","");
            return true;
        }
        if(code==android.view.KeyEvent.KEYCODE_ENTER || code==android.view.KeyEvent.KEYCODE_SPACE
                || code==android.view.KeyEvent.KEYCODE_DPAD_CENTER || code==android.view.KeyEvent.KEYCODE_BUTTON_A){
            if(event.getAction()==android.view.KeyEvent.ACTION_UP)enqueue("show_interface","","");
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed(){enqueue("back","","");}
    private void acceptIntent(Intent intent) {
        if (intent!=null && Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData()!=null)
            enqueue("open",intent.getData().toString(),"External video");
    }
    private synchronized void enqueue(String type,String value,String name) {if(events.size()<16)events.add(new String[]{type,value,name});}
    public synchronized String[] pollEvent() {return events.poll();}
    public void requestTree() {runOnUiThread(()->{
        try {Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(intent,200);
        } catch(Exception e){enqueue("error","Android's folder picker is unavailable.","");}
    });}
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
        if(request==200 && result==RESULT_OK && data!=null && data.getData()!=null){
            try {DocumentBrowser.persistReadGrant(getContentResolver(),data.getData(),data.getFlags());
                getSharedPreferences("player",0).edit().putString("tree",data.getData().toString()).apply();
                enqueue("tree",data.getData().toString(),"Authorized folder");
            }catch(Exception e){enqueue("error","Android denied access to this folder.","");}
        }
    }
    public String savedTree(){
        android.content.SharedPreferences shared=getSharedPreferences("player",0);
        if(!shared.contains("tree")){
            String previous=getSharedPreferences("PlayerActivity",0).getString("tree","");
            if(!previous.isEmpty())shared.edit().putString("tree",previous).apply();
        }
        return shared.getString("tree","");
    }
    public String exportCertificates() throws Exception {return SystemCertificates.exportPem(this);}
    public String[] folderLocations(){
        ArrayList<String> values=new ArrayList<>();
        for(android.content.UriPermission grant:getContentResolver().getPersistedUriPermissions()){
            if(!grant.isReadPermission() || !android.provider.DocumentsContract.isTreeUri(grant.getUri()))continue;
            String name=android.provider.DocumentsContract.getTreeDocumentId(grant.getUri());
            int split=Math.max(name.lastIndexOf('/'),name.lastIndexOf(':'));
            if(split>=0 && split+1<name.length())name=name.substring(split+1);
            if(name.endsWith(":"))name=name.substring(0,name.length()-1);
            if(name.isEmpty())name="Folder";
            values.add(grant.getUri().toString());values.add(name);
        }
        return values.toArray(new String[0]);
    }
    public byte[] videoThumbnail(String location){
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
        android.graphics.Bitmap bitmap=null;
        try{
            Uri uri=Uri.parse(location);
            if("content".equals(uri.getScheme())){
                CancellationSignal signal=new CancellationSignal();
                java.util.concurrent.ScheduledFuture<?> deadline=thumbnailTimer.schedule(signal::cancel,1500,java.util.concurrent.TimeUnit.MILLISECONDS);
                try{bitmap=getContentResolver().loadThumbnail(uri,new android.util.Size(160,90),signal);}catch(Exception ignored){}finally{deadline.cancel(false);}
            }
            if(bitmap==null){
                try(android.media.MediaMetadataRetriever retriever=new android.media.MediaMetadataRetriever()){
                    if("content".equals(uri.getScheme()))retriever.setDataSource(this,uri);
                    else try(java.io.FileInputStream input=new java.io.FileInputStream(location)){retriever.setDataSource(input.getFD());}
                    android.media.MediaMetadataRetriever.BitmapParams params=new android.media.MediaMetadataRetriever.BitmapParams();params.setPreferredConfig(android.graphics.Bitmap.Config.ARGB_8888);
                    for(long time:new long[]{1000000,0,-1}){bitmap=retriever.getScaledFrameAtTime(time,android.media.MediaMetadataRetriever.OPTION_CLOSEST_SYNC,160,90,params);if(bitmap!=null)break;}
                }
            }
            if(bitmap==null)return null;
            android.graphics.Bitmap fitted=android.graphics.Bitmap.createBitmap(160,90,android.graphics.Bitmap.Config.ARGB_8888);
            android.graphics.Canvas canvas=new android.graphics.Canvas(fitted);canvas.drawColor(android.graphics.Color.BLACK);
            float scale=Math.min(160f/bitmap.getWidth(),90f/bitmap.getHeight());float w=bitmap.getWidth()*scale,h=bitmap.getHeight()*scale;
            canvas.drawBitmap(bitmap,null,new android.graphics.RectF((160-w)/2,(90-h)/2,(160+w)/2,(90+h)/2),new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG));
            int[] colors=new int[160*90];fitted.getPixels(colors,0,160,0,0,160,90);fitted.recycle();
            byte[] rgba=new byte[160*90*4];for(int i=0;i<colors.length;i++){int c=colors[i];rgba[i*4]=(byte)(c>>16);rgba[i*4+1]=(byte)(c>>8);rgba[i*4+2]=(byte)c;rgba[i*4+3]=(byte)255;}return rgba;
        }catch(Exception error){return null;}finally{if(bitmap!=null)bitmap.recycle();}
    }
    public String hardwareCodec(String mime,int width,int height){
        String[] names=CodecProbe.hardwareCandidates(MediaFormat.createVideoFormat(mime,width,height));
        return names.length==0?"":names[0];
    }
    public String[] listDocuments(String uri) throws Exception {
        CancellationSignal signal=new CancellationSignal();synchronized(this){query=signal;}
        try {
            DocumentBrowser.Listing listing=DocumentBrowser.listChildren(getContentResolver(),Uri.parse(uri),10000,"",signal);
            ArrayList<String> flat=new ArrayList<>();
            for(DocumentBrowser.Entry e:listing.entries){
                flat.add(e.uri.toString());flat.add(e.name);flat.add(e.directory?"1":"0");flat.add(Long.toString(Math.max(0,e.size)));
            }
            return flat.toArray(new String[0]);
        } finally {synchronized(this){if(query==signal)query=null;}}
    }
    public void cancelQuery(){final CancellationSignal signal;synchronized(this){signal=query;}if(signal!=null)cancellationWorker.execute(signal::cancel);}
    public long[] openDocument(String uri) throws Exception {
        DocumentBrowser.OpenDocument doc=DocumentBrowser.open(getContentResolver(),Uri.parse(uri),null);
        synchronized(this){openDocuments.add(doc);}
        return new long[]{doc.fd,doc.offset,doc.length};
    }
    public void closeDocument(int fd) throws Exception {
        DocumentBrowser.OpenDocument doc=null;
        synchronized(this){for(int i=0;i<openDocuments.size();i++)if(openDocuments.get(i).fd==fd){doc=openDocuments.remove(i);break;}}
        if(doc!=null)doc.close();
    }
    @Override public void onDestroy(){super.onDestroy();cancellationWorker.shutdown();thumbnailTimer.shutdownNow();}
}
