package com.example.gstreamer;

import androidx.appcompat.app.AppCompatActivity;
import android.content.Context;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;

import com.github.anastr.speedviewlib.ImageSpeedometer;
import com.github.anastr.speedviewlib.SpeedView;
import java.util.Random;

public class BpsView extends AppCompatActivity {
    public static Context context;
    private ImageSpeedometer speedMeter;
    private float fValue;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_bps_view);

        context = this;
        speedMeter = (ImageSpeedometer) findViewById(R.id.bpsView);

        speedMeter.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
    }

    public void update(float v) {
        fValue = v;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                speedMeter.speedTo(fValue*1000.0F);
            }
        });
    }
}