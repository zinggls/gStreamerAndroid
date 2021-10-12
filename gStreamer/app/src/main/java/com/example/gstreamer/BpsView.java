package com.example.gstreamer;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.view.View;
import android.widget.Button;

import com.github.anastr.speedviewlib.SpeedView;

public class BpsView extends AppCompatActivity {
    private Button btnMain;
    private SpeedView speedMeter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_bps_view);

        btnMain = (Button) findViewById(R.id.button_main);
        speedMeter = (SpeedView) findViewById(R.id.bpsView);
        speedMeter.speedTo(50);

        btnMain.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
    }
}