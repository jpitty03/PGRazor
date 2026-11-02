package dev.pgrazor;

import android.content.Context;
import android.graphics.Canvas;
import android.view.View;
import android.widget.Toast;

/** Draggable map handle; Pad owns its OverlayDrag, visibility, scale, and lifecycle. */
public final class GymFeedOverlay extends View {
    private final OverlayToggleArt art=new OverlayToggleArt();

    public GymFeedOverlay(Context context){
        super(context);
        setOutlineProvider(SettingsUi.roundRectOutline(16f/54f));
        setClickable(true);setFocusable(true);
        setOnClickListener(v->{
            boolean wasOn=GymAutomationSettings.running();
            boolean nowOn=GymAutomationSettings.toggleRunning();
            if(!wasOn&&!nowOn)
                Toast.makeText(getContext(),GymAutomationSettings.status(),Toast.LENGTH_SHORT).show();
            refresh(true);
        });
    }

    /** Pad calls this on each heartbeat, hiding it beneath settings and quick menu. */
    public void refresh(boolean visible){
        boolean on=GymAutomationSettings.running();
        setVisibility(visible&&GymAutomationSettings.overlayEnabled()?VISIBLE:GONE);
        setContentDescription("Gym Slotter and Berry Feeder "+(on?"on; tap to stop":"off; tap to start")
            +". Drag to reposition. "+GymAutomationSettings.status());
        invalidate();
    }

    public void close(){setOnClickListener(null);setVisibility(GONE);}

    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);canvas.save();canvas.scale(getWidth()/54f,getHeight()/54f);
        art.gym(canvas,GymAutomationSettings.running(),Cooldown.remaining()>0);canvas.restore();
    }
}
