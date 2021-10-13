package com.example.gstreamer;

import androidx.appcompat.app.AppCompatActivity;
import android.content.Context;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;

import com.github.anastr.speedviewlib.SpeedView;
import java.util.Random;

public class BpsView extends AppCompatActivity {
    public static Context context;
    private Button btnMain;
    private Button btnUpdate;
    private SpeedView speedMeter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_bps_view);

        context = this;
        btnMain = (Button) findViewById(R.id.button_main);
        speedMeter = (SpeedView) findViewById(R.id.bpsView);
        speedMeter.setUnit("Gbps");
        speedMeter.setMinSpeed(0.0F);
        speedMeter.setMaxSpeed(2.5F);
        speedMeter.speedTo(0.0F);
        speedMeter.setWithTremble(false);
        btnUpdate = (Button) findViewById(R.id.button_update);

        btnMain.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });

        btnUpdate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Random random = new Random();
                speedMeter.speedTo(random.nextFloat());
            }
        });
    }

    public void UpdateButton() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                btnUpdate.performClick();
            }
        });
    }
}