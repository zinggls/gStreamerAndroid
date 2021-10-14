package com.example.gstreamer;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

import android.Manifest;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import com.github.mikephil.charting.charts.LineChart;
import com.github.mikephil.charting.components.Description;
import com.github.mikephil.charting.components.XAxis;
import com.github.mikephil.charting.components.YAxis;
import com.github.mikephil.charting.data.Entry;
import com.github.mikephil.charting.data.LineData;
import com.github.mikephil.charting.data.LineDataSet;
import com.github.mikephil.charting.formatter.IndexAxisValueFormatter;
import com.github.mikephil.charting.formatter.LargeValueFormatter;
import com.github.mikephil.charting.formatter.ValueFormatter;
import com.github.mikephil.charting.interfaces.datasets.ILineDataSet;
import com.github.mikephil.charting.utils.ViewPortHandler;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import com.example.gstreamer.RealPathUtil;

public class MainActivity extends AppCompatActivity {

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("glsusb");
    }

    private Button btnCon;
    private Button btnSnd;
    private Button btnRcv;
    private boolean receiveBtn=false;
    private boolean selfSndBtn=false;
    private static final String TAG = "glsusb";
    private static final String ACTION_USB_PERMISSION = "com.example.gstreamer.USB_PERMISSION";
    private UsbManager usbManager;
    private int fileDescriptor;
    public static final int PRODUCT_ID = 0x00f0;    //FX3
    public static final int VENDOR_ID = 0x04b4;     //Cypress
    private static Handler handler;
    private ListView list;
    List<String> listData;
    ArrayAdapter<String> listAdapter;
    private ProgressBar pgFile;
    private LineChart chart;

    public native int open(int fileDescriptor);
    public native void close();
    public native int reader();
    public native int stopReader();
    public native long count();
    public native int writer(Object fileList);
    public native int stopWriter();
    public native int bps();

    private void createList()
    {
        list = (ListView)findViewById(R.id.list);
        listData = new ArrayList<>();
        listAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1,listData);
        list.setAdapter(listAdapter);
    }

    private void createButtons()
    {
        btnCon = (Button)findViewById(R.id.button_Connect);
        btnSnd = (Button)findViewById(R.id.button_Send);
        btnRcv = (Button)findViewById(R.id.button_Recv);
        btnCon.setEnabled(true);
        btnSnd.setEnabled(false);
        btnRcv.setEnabled(false);
    }

    private void createProgressbar()
    {
        pgFile = (ProgressBar)findViewById(R.id.progressBarFile);
        pgFile.setMax(100);
        pgFile.setProgress(0);
        pgFile.setVisibility(View.INVISIBLE);
    }

    private void createChart()
    {
        chart = (LineChart)findViewById(R.id.LineChart);
        chart.getDescription().setEnabled(false);

        XAxis xAxis = chart.getXAxis();
        xAxis.setDrawLabels(false);
        xAxis.setDrawAxisLine(true);
        xAxis.setDrawGridLines(true);

        YAxis rightAxis = chart.getAxisRight();
        rightAxis.setEnabled(false);

        YAxis leftAxis = chart.getAxisLeft();
        leftAxis.setLabelCount(5);
        leftAxis.setAxisMinimum(0.0f);
        leftAxis.setAxisMaximum(2.5f);

        ArrayList<Entry> values = new ArrayList<>();
        values.add(new Entry(0,0f));

        LineDataSet set1 = new LineDataSet(values,"Gbps");
        set1.setColor(Color.RED);
        set1.setDrawCircles(false);
        set1.setDrawValues(false);

        ArrayList<ILineDataSet> dataSets = new ArrayList<>();
        dataSets.add(set1);

        LineData data = new LineData(dataSets);
        chart.setData(data);
        chart.setVisibility(View.INVISIBLE);
    }

    private LineDataSet createSet()
    {
        LineDataSet set = new LineDataSet(null,"bps");
        //set.setFillAlpha(110);
        return set;
    }

    private void addEntry(float bpsVal)
    {
        LineData data = chart.getData();
        if(data==null) chart.setData(new LineData());

        ILineDataSet set = data.getDataSetByIndex(0);
        assert(set!=null);
        //if(set==null) data.addDataSet(createSet());

        data.addEntry(new Entry((float)set.getEntryCount(),bpsVal),0);
        data.notifyDataChanged();
        chart.notifyDataSetChanged();
        chart.setVisibleXRangeMaximum(50);
        chart.moveViewTo(data.getEntryCount(),0f, YAxis.AxisDependency.LEFT);
    }

    private boolean listDataAdd(String msg)
    {
        if(listData.add(msg)) {
            listAdapter.notifyDataSetChanged();
            return true;
        }
        return false;
    }

    private int LI(String tag,String msg)
    {
        listDataAdd(msg);
        return Log.i(tag,msg);
    }

    private int LE(String tag,String msg)
    {
        listDataAdd(msg);
        return Log.e(tag,msg);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        ((TextView)findViewById(R.id.tvHello)).setText(stringFromJNI());

        createButtons();
        createList();
        createProgressbar();
        createChart();

        handler = new Handler(Looper.getMainLooper()){
            @Override
            public void handleMessage(Message msg){
                ((TextView)findViewById(R.id.tvHello)).setText("count:"+msg.obj);
            }
        };

        class NewRunnerable implements Runnable{
            @Override
            public void run() {
                while(true){
                    try{
                        Thread.sleep(100);
                    }catch(Exception e){
                        e.printStackTrace();
                    }
                    Message msg = new Message();
                    msg.obj = Long.toString(count());
                    handler.sendMessage(msg);

                    int bpsVal = bps();
                    //Log.i(TAG,"bps()="+bpsVal);
                    float value = (float) (((float)bpsVal)/1000000000.0);
                    //Log.i(TAG,"v="+value);
                    addEntry(value);
                    if(BpsView.context!=null) ((BpsView)BpsView.context).UpdateButton(value);
                }
            }
        };

        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        PendingIntent permissionIntent = PendingIntent.getBroadcast(this, 0, new Intent(ACTION_USB_PERMISSION), 0);
        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        registerReceiver(usbReceiver, filter);

        btnCon.setOnClickListener(new View.OnClickListener(){
            @Override
            public void onClick(View v) {
                LI(TAG,"Connect button clicked");

                if(usbManager==null) {
                    LE(TAG,"usbManager = null");
                    return;
                }

                HashMap<String, UsbDevice> deviceList = usbManager.getDeviceList();
                if(deviceList == null) {
                    LE(TAG,"deviceList = null");
                    return;
                }
                LI(TAG,"deviceList="+deviceList.size());

                Iterator<UsbDevice> deviceIterator = deviceList.values().iterator();
                while(deviceIterator.hasNext()) {
                    UsbDevice device = deviceIterator.next();
                    LI(TAG,"deviceName="+device.getDeviceName());

                    if(device.getProductId()==PRODUCT_ID && device.getVendorId()==VENDOR_ID) {
                        LI(TAG,"FX3 found (ProductID=0x00f0,VendorID=0x04b4)");

                        UsbDeviceConnection usbDeviceConnection = usbManager.openDevice(device);
                        if(usbDeviceConnection==null) {
                            LE(TAG,"usbDeviceConnection = null");
                        }else{
                            fileDescriptor = usbDeviceConnection.getFileDescriptor();
                            LI(TAG,"fileDescriptor="+fileDescriptor);

                            usbManager.requestPermission(device,permissionIntent);
                            LI(TAG,"requestPermission done");

                            NewRunnerable nr= new NewRunnerable();
                            (new Thread(nr)).start();
                        }
                    }
                }
            }
        });

        btnRcv.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                btnCon.setEnabled(false);
                btnSnd.setEnabled(false);
                btnRcv.setEnabled(true);
                LI(TAG, "Receive button clicked");

                int r;
                if(receiveBtn){
                    btnSnd.setEnabled(true);
                    btnRcv.setText("Recv");
                    chart.setVisibility(View.VISIBLE);
                    stopReader();
                    LI(TAG, "Reader stopped");
                }else{
                    btnSnd.setEnabled(false);
                    btnRcv.setText("StopRecv");
                    chart.setVisibility(View.VISIBLE);
                    if((r=reader())==0) {
                        LI(TAG, "Reader starts successfully");
                    }else{
                        LI(TAG, "Reader failed to start, error=" + r);
                    }
                }
                receiveBtn = !receiveBtn;
            }
        });

        btnSnd.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if(selfSndBtn) {
                    //TODO
                    stopWriter();
                    btnSnd.setEnabled(true);
                    btnRcv.setEnabled(true);
                    btnSnd.setText("Send");
                    LI(TAG, "Writer stopped");
                    selfSndBtn = !selfSndBtn;
                    return;
                }
                btnCon.setEnabled(false);
                btnSnd.setEnabled(false);
                btnRcv.setEnabled(false);
                btnSnd.setText("Sending");
                LI(TAG, "Send button clicked");

                AlertDialog.Builder ad = new AlertDialog.Builder(MainActivity.this);
                ad.setIcon(R.mipmap.ic_launcher);
                ad.setTitle("Select Data Source");
                ad.setMessage(" Choose data source for streaming\n\nSelf Mode: self generated data\nFile Mode: data from file");

                ad.setPositiveButton("File mode", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        LI(TAG, "File mode selected");

                        Intent chooseFile = new Intent(Intent.ACTION_GET_CONTENT);
                        chooseFile.setType("*/*");
                        chooseFile.putExtra(Intent.EXTRA_ALLOW_MULTIPLE,true);
                        startActivityForResult(Intent.createChooser(chooseFile,"Choose a file"),0);
                        dialog.dismiss();
                    }
                });

                ad.setNegativeButton("Self mode", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if(!selfSndBtn){
                            btnSnd.setEnabled(true);
                            btnRcv.setEnabled(false);
                            btnSnd.setText("StopSend");
                            LI(TAG, "Self mode selected");
                            int r = writer(null);
                            if(r==0) {
                                LI(TAG, "Writer starts successfully");
                            }else{
                                LE(TAG, "Writer failed to start, error=" + r);
                            }
                            dialog.dismiss();
                        }
                        selfSndBtn = !selfSndBtn;
                    }
                });
                ad.show();
            }
        });

        chart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent intent = new Intent(getApplicationContext(),BpsView.class);
                startActivity(intent);
                overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
            }
        });

        isStoragePermissionGranted();
    }

    @Override
    protected void onDestroy() {
        close();
        LI(TAG, "onDestroy");
        super.onDestroy();
    }

    /**
     * Get a file path from a Uri. This will get the the path for Storage Access
     * Framework Documents, as well as the _data field for the MediaStore and
     * other file-based ContentProviders.
     *
     * @param context The context.
     * @param uri The Uri to query.
     * @author paulburke
     */
    public static String getPath(final Context context, final Uri uri) {

        final boolean isKitKat = Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT;

        // DocumentProvider
        if (isKitKat && DocumentsContract.isDocumentUri(context, uri)) {
            // ExternalStorageProvider
            if (isExternalStorageDocument(uri)) {
                final String docId = DocumentsContract.getDocumentId(uri);
                final String[] split = docId.split(":");
                final String type = split[0];

                if ("primary".equalsIgnoreCase(type)) {
                    return Environment.getExternalStorageDirectory() + "/" + split[1];
                }

                // TODO handle non-primary volumes
            }
            // DownloadsProvider
            else if (isDownloadsDocument(uri)) {

                final String id = DocumentsContract.getDocumentId(uri);
                final Uri contentUri = ContentUris.withAppendedId(
                        Uri.parse("content://downloads/public_downloads"), Long.valueOf(id));

                return getDataColumn(context, contentUri, null, null);
            }
            // MediaProvider
            else if (isMediaDocument(uri)) {
                final String docId = DocumentsContract.getDocumentId(uri);
                final String[] split = docId.split(":");
                final String type = split[0];

                Uri contentUri = null;
                if ("image".equals(type)) {
                    contentUri = MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
                } else if ("video".equals(type)) {
                    contentUri = MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
                } else if ("audio".equals(type)) {
                    contentUri = MediaStore.Audio.Media.EXTERNAL_CONTENT_URI;
                }

                final String selection = "_id=?";
                final String[] selectionArgs = new String[] {
                        split[1]
                };

                return getDataColumn(context, contentUri, selection, selectionArgs);
            }
        }
        // MediaStore (and general)
        else if ("content".equalsIgnoreCase(uri.getScheme())) {
            return getDataColumn(context, uri, null, null);
        }
        // File
        else if ("file".equalsIgnoreCase(uri.getScheme())) {
            return uri.getPath();
        }

        return null;
    }

    /**
     * Get the value of the data column for this Uri. This is useful for
     * MediaStore Uris, and other file-based ContentProviders.
     *
     * @param context The context.
     * @param uri The Uri to query.
     * @param selection (Optional) Filter used in the query.
     * @param selectionArgs (Optional) Selection arguments used in the query.
     * @return The value of the _data column, which is typically a file path.
     */
    public static String getDataColumn(Context context, Uri uri, String selection,
                                       String[] selectionArgs) {

        Cursor cursor = null;
        final String column = "_data";
        final String[] projection = {
                column
        };

        try {
            cursor = context.getContentResolver().query(uri, projection, selection, selectionArgs,
                    null);
            if (cursor != null && cursor.moveToFirst()) {
                final int column_index = cursor.getColumnIndexOrThrow(column);
                return cursor.getString(column_index);
            }
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return null;
    }


    /**
     * @param uri The Uri to check.
     * @return Whether the Uri authority is ExternalStorageProvider.
     */
    public static boolean isExternalStorageDocument(Uri uri) {
        return "com.android.externalstorage.documents".equals(uri.getAuthority());
    }

    /**
     * @param uri The Uri to check.
     * @return Whether the Uri authority is DownloadsProvider.
     */
    public static boolean isDownloadsDocument(Uri uri) {
        return "com.android.providers.downloads.documents".equals(uri.getAuthority());
    }

    /**
     * @param uri The Uri to check.
     * @return Whether the Uri authority is MediaProvider.
     */
    public static boolean isMediaDocument(Uri uri) {
        return "com.android.providers.media.documents".equals(uri.getAuthority());
    }

    private String getRealPathFromURI(Uri contentUri) {
        if (contentUri.getPath().startsWith("/storage")) {
            return contentUri.getPath();
        }
        String id = DocumentsContract.getDocumentId(contentUri).split(":")[1];
        if (id.startsWith("/storage")) return id;

        String[] columns = { MediaStore.Files.FileColumns.DATA };
        String selection = MediaStore.Files.FileColumns._ID + " = " + id;
        Cursor cursor = getContentResolver().query(MediaStore.Files.getContentUri("external"), columns, selection, null, null);
        try {
            int columnIndex = cursor.getColumnIndex(columns[0]);
            if (cursor.moveToFirst()) {
                return cursor.getString(columnIndex);
            }
        } finally {
            cursor.close();
        }
        return null;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if(requestCode == 0 && resultCode == RESULT_OK) {
            //LI(TAG, "onActivityResult requestCode is 0 and resultCode is RESULT_OK");

            ArrayList<String> fileList = new ArrayList<String>();
            ClipData clip = data.getClipData();
            if(clip!=null) {
                LI(TAG, "ItemCount="+clip.getItemCount());
                for(int i=0;i<clip.getItemCount();i++) {
                    ClipData.Item item = clip.getItemAt(i);
                    Uri uri = item.getUri();
                    fileList.add(RealPathUtil.getRealPath(getApplicationContext(),uri));
                }
            }else{
                //When only one is selected, ClipData is null. data.getData() will be the Uri of the selected one
                fileList.add(RealPathUtil.getRealPath(getApplicationContext(),data.getData()));
            }

            if(fileList.size()>0) {
                int r = writer(fileList);
                if(r==0) {
                    pgFile.setVisibility(View.VISIBLE);
                    LI(TAG, "Writer starts successfully");
                }else{
                    LE(TAG, "Writer failed to start, error=" + r);
                }
            }else{
                LE(TAG, "File list is empty," + fileList.size());
            }
        }
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
    }

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        public void onReceive(Context context, Intent intent) {
            LI(TAG, "BroadcastReceiver.onReceive called");

            String action = intent.getAction();
            LI(TAG, "action="+action);
            if (ACTION_USB_PERMISSION.equals(action)) {
                synchronized (this) {
                    UsbDevice device = (UsbDevice)intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);

                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        if(device != null){
                            //call method to set up device communication
                            LI(TAG, "permission granted for device ");

                            int r = open(fileDescriptor);
                            if(r==0) {
                                LI(TAG,"Device open successful");

                                btnCon.setEnabled(false);
                                btnSnd.setEnabled(true);
                                btnRcv.setEnabled(true);
                                btnCon.setText("Connected");
                                chart.setVisibility(View.VISIBLE);
                                pgFile.setVisibility(View.INVISIBLE);
                            }else{
                                LE(TAG, "Device open failure, error=" + r);
                            }
                        }else
                            LE(TAG, "device = null");
                    }
                    else {
                        LE(TAG, "permission denied for device " + device);
                    }
                }
            }else if (action.equals(UsbManager.ACTION_USB_DEVICE_ATTACHED)) {
                LI(TAG, "ACTION_USB_DEVICE_ATTACHED");
            }else if (action.equals(UsbManager.ACTION_USB_DEVICE_DETACHED)){
                LI(TAG, "ACTION_USB_DEVICE_DETACHED");

                if(receiveBtn) {
                    btnRcv.setText("Recv");
                    stopReader();
                    receiveBtn = false;
                }
                close();
                btnCon.setEnabled(true);
                btnSnd.setEnabled(false);
                btnRcv.setEnabled(false);
                btnCon.setText("Connect");
                chart.setVisibility(View.VISIBLE);
                pgFile.setVisibility(View.INVISIBLE);
                LI(TAG,"Device closed");
            }
        }
    };

    public boolean isStoragePermissionGranted()
    {
        if (Build.VERSION.SDK_INT >= 23)
        {
            if (checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED)
            {
                LI(TAG,"Permission is granted");
                return true;
            } else {
                LI(TAG,"Permission is revoked");
                ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
                return false;
            }
        } else {
            //permission is automatically granted on sdk<23 upon installation
            LI(TAG,"Permission is granted");
            return true;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults)
    {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if(grantResults[0]== PackageManager.PERMISSION_GRANTED)
        {
            LI(TAG,"Permission: "+permissions[0]+ " was "+grantResults[0]);
            Toast.makeText(this, "File access permission granted.", Toast.LENGTH_SHORT).show();
        } else {
            Toast.makeText(this, "need file access permission.", Toast.LENGTH_LONG).show();
        }
    }

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    public native String stringFromJNI();

    public void onMessage(String s)
    {
        Log.i(TAG,"onMessage");
        handler.post(new Runnable() {
            @Override
            public void run() {
                LI(TAG,s);
            }
        });
    }

    public void onFileStartReceiving(String s)
    {
        Log.i(TAG,"onFileStartReceiving");
        handler.post(new Runnable() {
            @Override
            public void run() {
                pgFile.setVisibility(View.VISIBLE);
            }
        });
    }

    public void onFileReceived(String s)
    {
        Log.i(TAG,"onFileReceived");
        handler.post(new Runnable() {
            @Override
            public void run() {
                pgFile.setVisibility(View.INVISIBLE);
                LI(TAG,"'"+s+"' received");
            }
        });
    }

    public void onFileSent(String s)
    {
        Log.i(TAG,"onFileSent");
        handler.post(new Runnable() {
            @Override
            public void run() {
                LI(TAG,"'"+s+"' sent");
            }
        });
    }

    public void onAllFilesSent(String s)
    {
        Log.i(TAG,"onAllFilesSent");
        handler.post(new Runnable() {
            @Override
            public void run() {
                btnCon.setEnabled(false);
                btnSnd.setEnabled(true);
                btnRcv.setEnabled(true);
                btnSnd.setText("Send");
                pgFile.setVisibility(View.INVISIBLE);
                LI(TAG,s);
            }
        });
    }

    public void onFileProgress(int percentage)
    {
        Log.i(TAG,"onFileProgress="+percentage);
        handler.post(new Runnable() {
            @Override
            public void run() {
                pgFile.setProgress(percentage);
            }
        });
    }

    public int onFileName(String s)
    {
        Log.i(TAG,"onFileName=" + s);

        ContentValues values = new ContentValues();
        values.put(MediaStore.Video.Media.DISPLAY_NAME, s);
        values.put(MediaStore.Video.Media.MIME_TYPE, "video/*");

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            values.put(MediaStore.Video.Media.IS_PENDING, 1);
        }

        ContentResolver contentResolver = getContentResolver();
        Uri item = contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);

        int fd = -1;
        try{
            ParcelFileDescriptor parcelFd = contentResolver.openFileDescriptor(item, "w");
            if (parcelFd != null) {
                fd = parcelFd.detachFd();
                Log.i(TAG,"detachFd=" + fd);

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    values.clear();
                    values.put(MediaStore.Images.Media.IS_PENDING, 0);
                    contentResolver.update(item, values, null, null);
                }
            }
        }catch(FileNotFoundException e){
            e.printStackTrace();
        }
        return fd;
    }
}