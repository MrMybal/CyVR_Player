package com.cyvr.player;

import android.content.ContentResolver;
import android.content.Intent;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.provider.DocumentsContract;
import java.io.Closeable;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** Data provider for the future internal browser. No Activity or widgets.
 * The app first obtains a tree grant from Android. URI permissions are enforced
 * by ContentResolver; a content URI is never converted to a guessed file path. */
public final class DocumentBrowser {
    private DocumentBrowser() {}
    public static final class Entry {
        public final Uri uri;
        public final String name, mimeType;
        public final boolean directory;
        public final long size, modifiedMillis;
        Entry(Uri uri, String name, String mime, long size, long modified) {
            this.uri=uri; this.name=name; this.mimeType=mime;
            this.directory=DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
            this.size=size; this.modifiedMillis=modified;
        }
    }
    public static final class Listing {
        public final List<Entry> entries;
        public final boolean truncated;
        Listing(List<Entry> entries, boolean truncated) {
            this.entries=Collections.unmodifiableList(entries); this.truncated=truncated;
        }
    }
    public static Listing listChildren(ContentResolver resolver, Uri directory,
            int limit, String search, CancellationSignal cancel) throws IOException {
        if (limit<1 || limit>10000 || !DocumentsContract.isTreeUri(directory))
            throw new IllegalArgumentException("A granted document tree and a bounded limit are required");
        String id=directory.getPathSegments().contains("document")
            ? DocumentsContract.getDocumentId(directory) : DocumentsContract.getTreeDocumentId(directory);
        Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(directory,id);
        String[] columns={DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,DocumentsContract.Document.COLUMN_LAST_MODIFIED};
        ArrayList<Entry> entries=new ArrayList<>();
        boolean truncated=false;
        String filter=search==null ? "" : search.toLowerCase(java.util.Locale.ROOT);
        int scanned=0;
        try (Cursor cursor=resolver.query(children,columns,null,null,null,cancel)) {
            if (cursor==null) throw new IOException("Document provider returned no cursor");
            while (cursor.moveToNext()) {
                if (cancel!=null) cancel.throwIfCanceled();
                if (++scanned>100000) { truncated=true; break; }
                String name=cursor.isNull(1) ? "" : cursor.getString(1);
                if (!name.toLowerCase(java.util.Locale.ROOT).contains(filter)) continue;
                if (entries.size()==limit) { truncated=true; break; }
                Uri uri=DocumentsContract.buildDocumentUriUsingTree(directory,cursor.getString(0));
                entries.add(new Entry(uri,name,cursor.getString(2),
                    cursor.isNull(3) ? -1 : cursor.getLong(3),cursor.isNull(4) ? -1 : cursor.getLong(4)));
            }
        }
        Collections.sort(entries,new Comparator<Entry>() {
            @Override public int compare(Entry a,Entry b) {
                if (a.directory!=b.directory) return a.directory ? -1 : 1;
                int order=a.name.compareToIgnoreCase(b.name);
                return order==0 ? a.name.compareTo(b.name) : order;
            }
        });
        return new Listing(entries,truncated);
    }
    public static void persistReadGrant(ContentResolver resolver, Uri tree, int returnedFlags) {
        if ((returnedFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION)==0)
            throw new SecurityException("Android did not grant read access");
        resolver.takePersistableUriPermission(tree,Intent.FLAG_GRANT_READ_URI_PERMISSION);
    }
    public static final class OpenDocument implements Closeable {
        private final AssetFileDescriptor asset;
        public final int fd;
        public final long offset, length;
        private OpenDocument(AssetFileDescriptor asset) {
            this.asset=asset; this.fd=asset.getParcelFileDescriptor().getFd();
            this.offset=asset.getStartOffset(); this.length=asset.getDeclaredLength();
        }
        // Keep this open until the native caller has duplicated fd via
        // Demuxer.open(MediaSource.descriptor(...)); then Java can close it.
        @Override public void close() throws IOException { asset.close(); }
    }
    public static OpenDocument open(ContentResolver resolver, Uri uri, CancellationSignal cancel) throws IOException {
        AssetFileDescriptor asset=resolver.openAssetFileDescriptor(uri,"r",cancel);
        if (asset==null) throw new IOException("Document provider could not open the file");
        return new OpenDocument(asset);
    }
}
