package dev.pgrazor;

import android.app.*;
import android.content.*;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.os.*;
import android.text.InputType;
import android.view.*;
import android.widget.*;
import java.util.Locale;

/**
 * The full-screen PGRazor settings body: every switch, checkbox, status caption and section,
 * plus their construction and per-frame refresh. Extracted from {@link Pad}, which keeps overlay
 * attachment/lifecycle, the launcher/quick menu and the joystick. This panel never touches those
 * overlays directly; the few cross-cutting actions it triggers go through {@link Host} so Pad's
 * internals stay private. Settings keys and the {@link Control} API are unchanged.
 */
final class SettingsPanel {
    /** Narrow callback surface back into Pad: everything the settings body needs that lives with the overlays/joystick. */
    interface Host {
        /** Full refresh (overlays + this panel); matches Pad's existing single refresh entry point. */
        void refresh();
        /** Re-theme overlays and this panel after a dark/light toggle. */
        void applyTheme();
        /** Release the location joystick, optionally preserving an in-progress walk. */
        void releaseJoystick(boolean preserveWalk);
        /** Persist the joystick-visible choice and reflect it on the overlay Pad owns. */
        void setJoystickVisible(boolean visible);
        /** Open the shared World map (also used by the quick menu). */
        void openMap();
        /** Open the coordinates & speed editor (tied to engine/joystick state Pad owns). */
        void editLocation();
    }

    private final Activity activity;
    private final Host host;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final java.util.List<AlertDialog> itemDialogs = new java.util.ArrayList<>();
    private final LinearLayout panel, body;
    private final ScrollView bodyScroll;
    private final TextView coords;
    private View closeIcon, themeIcon;
    private Switch incubatorSwitch;
    private Button incubatorPriority;
    private Button uiScale;
    private TextView incubatorStatus;
    // Primary action and cycle/dialog rows.
    private Button mapTap, throwGrade, itemSettings, cameraSample, interactionCapture, transferKeepList;
    // Boolean settings use proper switches.
    private Switch locationSwitch, vgpSwitch, joystickSwitch, cooldownSwitch, spawnSwitch, autoTransferSwitch,
        quickSpinSwitch, berrySwitch, quickSummarySwitch, curveSwitch, alwaysHitSwitch, arSwitch, ivCatchSwitch, ivOwnedSwitch, transferOverlaySwitch;
    private TextView quickSummaryLabel, vgpStatus;
    private final CheckBox[] vgpChoices=new CheckBox[3];
    private CheckBox vgpCooldownStop;
    // Status / caption lines (muted, readiness driven).
    private TextView state, steeringStatus, spawnStatus, quickSummaryStatus, transferStatus, quickSpinStatus,
        buddyStatus, giftStatus, itemStatus, rocketStatus, battleStatus, quickBallStatus, ivStatus, cameraStatus, interactionStatus;
    // Multi-option groups keep checkboxes / radios.
    private final CheckBox[] transferProtection=new CheckBox[6];
    private final CheckBox[] giftChoices=new CheckBox[3];
    private final CheckBox[] rocketChoices=new CheckBox[3];
    private final CheckBox[] battleChoices=new CheckBox[10];
    private Runnable buddyRefresh=()->{},smartRefresh=()->{};
    // Section roots gated by backend readiness, refreshed without recreation.
    private View catchRoot, buddyRoot, giftRoot, itemsRoot, rocketRoot, battleRoot, quickBallRoot;
    private LinearLayout cameraRoot, interactionRoot;
    private final java.util.List<TextView> sectionHeads=new java.util.ArrayList<>();
    private final java.util.List<TextView> captions=new java.util.ArrayList<>();

    private int dp(float x){return SettingsUi.dp(activity,x);}
    private int ink(){return SettingsUi.ink();}
    private int muted(){return SettingsUi.muted();}
    private int surface(){return SettingsUi.surface();}
    private int accent(){return SettingsUi.accent();}
    private GradientDrawable rounded(int color,int radius){return SettingsUi.rounded(activity,color,radius);}
    private void styleTree(View view){SettingsUi.styleTree(activity,view);}
    private Context dialogContext(){return SettingsUi.dialogContext(activity);}
    private TextView text(String label,int size,int color){return SettingsUi.text(activity,label,size,color);}
    private Button button(String label){return SettingsUi.button(activity,label);}

    /** A collapsible group. Content is built directly into the returned container, so no post-hoc index moves. */
    private LinearLayout section(LinearLayout owner,String label,boolean open){
        LinearLayout group=new LinearLayout(activity);group.setOrientation(LinearLayout.VERTICAL);
        LinearLayout head=new LinearLayout(activity);head.setOrientation(LinearLayout.HORIZONTAL);head.setGravity(Gravity.CENTER_VERTICAL);head.setMinimumHeight(dp(46));head.setClickable(true);head.setFocusable(true);
        TextView title=new TextView(activity);title.setText(label);title.setTextSize(14);head.addView(title,new LinearLayout.LayoutParams(0,-2,1f));
        TextView chev=new TextView(activity);chev.setTextSize(15);chev.setGravity(Gravity.CENTER);chev.setText(open?"–":"+");head.addView(chev,new LinearLayout.LayoutParams(dp(32),-2));
        LinearLayout content=new LinearLayout(activity);content.setOrientation(LinearLayout.VERTICAL);content.setPadding(dp(6),0,dp(2),dp(8));content.setVisibility(open?View.VISIBLE:View.GONE);
        head.setOnClickListener(v->{boolean o=content.getVisibility()!=View.VISIBLE;content.setVisibility(o?View.VISIBLE:View.GONE);chev.setText(o?"–":"+");});
        group.addView(head);group.addView(content);owner.addView(group);
        sectionHeads.add(title);sectionHeads.add(chev);
        return content;
    }
    private Switch switchRow(LinearLayout owner,String label){
        LinearLayout r=new LinearLayout(activity);r.setOrientation(LinearLayout.HORIZONTAL);r.setGravity(Gravity.CENTER_VERTICAL);r.setMinimumHeight(dp(44));
        TextView t=new TextView(activity);t.setText(label);t.setTextSize(14);r.addView(t,new LinearLayout.LayoutParams(0,-2,1f));
        Switch sw=new Switch(activity);sw.setContentDescription(label);sw.setClickable(true);r.addView(sw,new LinearLayout.LayoutParams(-2,-2));
        owner.addView(r);return sw;
    }
    private TextView caption(LinearLayout owner,String s){TextView t=new TextView(activity);t.setText(s);t.setTextSize(11);owner.addView(t);captions.add(t);return t;}

    /** Shows the destination theme: golden sun in dark mode, indigo moon in light mode. */
    private final class ThemeIcon extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        ThemeIcon(Context c){super(c);setFocusable(true);setClickable(true);setBackground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x3345e1ba),null,rounded(0xffffffff,24)));}
        protected void onDraw(Canvas c){super.onDraw(c);float x=getWidth()/2f,y=getHeight()/2f;
            if(Control.darkTheme()){
                paint.setColor(0xffffcf5c);paint.setStyle(Paint.Style.FILL);c.drawCircle(x,y,dp(5),paint);paint.setStrokeWidth(dp(2));paint.setStrokeCap(Paint.Cap.ROUND);
                for(int i=0;i<8;i++){double a=i*Math.PI/4;c.drawLine(x+(float)Math.cos(a)*dp(8),y+(float)Math.sin(a)*dp(8),x+(float)Math.cos(a)*dp(11),y+(float)Math.sin(a)*dp(11),paint);}
            }else{
                paint.setColor(0xff5355b9);paint.setStyle(Paint.Style.FILL);
                Path moon=new Path();moon.addCircle(x,y,dp(10),Path.Direction.CW);
                Path cut=new Path();cut.addCircle(x+dp(6),y-dp(4),dp(9),Path.Direction.CW);moon.op(cut,Path.Op.DIFFERENCE);c.drawPath(moon,paint);
            }
        }
    }
    /** A clean close affix: a small X drawn inside a 48dp hitbox. */
    private final class CloseIcon extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        CloseIcon(Context c){super(c);setContentDescription("Close PGRazor settings");setFocusable(true);setClickable(true);
            setBackground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x3345e1ba),null,rounded(0xffffffff,24)));}
        protected void onDraw(Canvas c){super.onDraw(c);float cx=getWidth()/2f,cy=getHeight()/2f,r=dp(8);paint.setStyle(Paint.Style.STROKE);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeWidth(dp(2));paint.setColor(muted());c.drawLine(cx-r,cy-r,cx+r,cy+r,paint);c.drawLine(cx-r,cy+r,cx+r,cy-r,paint);}
    }

    SettingsPanel(Activity activity, Host host) {
        this.activity = activity; this.host = host;
        panel = new LinearLayout(activity); panel.setOrientation(LinearLayout.VERTICAL); panel.setPadding(dp(12), dp(8), dp(12), dp(12));
        panel.setBackgroundColor(surface()); panel.setElevation(dp(12));
        // Full-screen settings header with theme and close controls.
        LinearLayout header = new LinearLayout(activity); header.setOrientation(LinearLayout.HORIZONTAL); header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("PGRazor", 19, accent()); title.setTypeface(Typeface.DEFAULT,Typeface.BOLD); title.setPadding(dp(4), dp(8), 0, dp(8)); sectionHeads.add(title);
        header.addView(title, new LinearLayout.LayoutParams(0, dp(48), 1f));
        themeIcon = new ThemeIcon(activity); header.addView(themeIcon,new LinearLayout.LayoutParams(dp(48),dp(48)));
        themeIcon.setOnClickListener(v -> {Control.setDarkTheme(!Control.darkTheme());host.applyTheme();});
        TextView version = text(BuildVersion.NAME, 12, muted());
        version.setSingleLine(true); version.setGravity(Gravity.CENTER_VERTICAL);
        version.setPadding(dp(4), 0, dp(8), 0);
        version.setContentDescription("PGRazor version " + BuildVersion.NAME);
        captions.add(version);
        header.addView(version, new LinearLayout.LayoutParams(-2, dp(48)));
        closeIcon = new CloseIcon(activity); header.addView(closeIcon, new LinearLayout.LayoutParams(dp(48), dp(48)));
        panel.addView(header);
        View gap = new View(activity); panel.addView(gap, new LinearLayout.LayoutParams(-1, dp(8)));
        body = new LinearLayout(activity); body.setOrientation(LinearLayout.VERTICAL);
        bodyScroll = new ScrollView(activity); bodyScroll.setFillViewport(false); bodyScroll.setVerticalScrollBarEnabled(false);
        bodyScroll.addView(body, new ScrollView.LayoutParams(-1,-2));
        panel.addView(bodyScroll, new LinearLayout.LayoutParams(-1,0,1));
        TextView discord = text("Join our Discord", 14, accent());
        discord.setGravity(Gravity.CENTER); discord.setMinimumHeight(dp(48));
        discord.setContentDescription("Join the PGRazor Discord community");
        discord.setClickable(true); discord.setFocusable(true); sectionHeads.add(discord);
        panel.addView(discord, new LinearLayout.LayoutParams(-1,dp(48)));
        discord.setOnClickListener(v -> {
            try { activity.startActivity(new Intent(Intent.ACTION_VIEW, android.net.Uri.parse("https://discord.gg/9QqfkDSQX3"))); }
            catch (android.content.ActivityNotFoundException unavailable) {
                android.widget.Toast.makeText(activity, "No app available to open Discord links", android.widget.Toast.LENGTH_SHORT).show();
            }
        });

        // Always-visible status and primary action.
        coords = text("", 16, ink()); coords.setPadding(dp(4), dp(6), dp(4), dp(10)); body.addView(coords);
        buildLocationSection();
        buildCatchSection();
        buildVgpSection();
        buildIncubationSection();
        buildBuddySection();
        buildGiftsSection();
        buildItemsSection();
        buildRocketSection();
        buildBattlesSection();
        buildThrowsSection();
        buildBackupSection();
        buildDiagnosticsSection();

        // Closing the full-screen settings panel simply returns to the game (or the quick menu on the next tap).
        closeIcon.setOnClickListener(v -> panel.setVisibility(View.GONE));
        // The settings panel is a full-screen modal now; it consumes taps so the game behind it stays inert.
        panel.setClickable(true);
    }

    private void buildLocationSection() {
        // ----- Location & overlays -----
        LinearLayout loc = section(body, "Location & overlays", true);
        uiScale=button("UI scale: "+Control.uiScalePercent()+"%");loc.addView(uiScale);
        uiScale.setOnClickListener(v->{final int[] values={100,75,50,25};String[] labels={"100%","75%","50%","25%"};int selected=0;for(int i=0;i<values.length;i++)if(values[i]==Control.uiScalePercent())selected=i;
            new AlertDialog.Builder(dialogContext()).setTitle("Overlay scale").setSingleChoiceItems(labels,selected,(dialog,which)->{host.releaseJoystick(true);Control.setUiScalePercent(values[which]);host.refresh();dialog.dismiss();}).setNegativeButton("Cancel",null).show();});
        locationSwitch=switchRow(loc,"Location spoofing");locationSwitch.setChecked(Control.desiredEnabled());
        locationSwitch.setOnCheckedChangeListener((v,checked)->{if(checked==Control.desiredEnabled())return;host.releaseJoystick(true);Control.setLocationSpoofing(checked);host.refresh();});
        steeringStatus = caption(loc, "Joystick follows the camera");
        Button position = button("Coordinates & speed"); loc.addView(position); position.setOnClickListener(v -> host.editLocation());
        Button map = button("World map"); loc.addView(map);
        map.setOnClickListener(v -> host.openMap());
        mapTap = button("Game map tap: Do nothing"); loc.addView(mapTap);
        mapTap.setOnClickListener(v -> { Control.setMapTapMode((Control.mapTapMode()+1)%4); host.refresh(); });
        spawnSwitch = switchRow(loc, "Spawn booster (80 m)"); spawnSwitch.setChecked(Control.spawnBooster());
        spawnSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.spawnBooster())return; Control.setSpawnBooster(spawnSwitch.isChecked()); });
        spawnStatus = caption(loc, "");
        cooldownSwitch = switchRow(loc, "Cooldown overlay"); cooldownSwitch.setChecked(Control.cooldownOverlay());
        cooldownSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.cooldownOverlay())return; Control.setCooldownOverlay(cooldownSwitch.isChecked()); host.refresh(); });
        Button history = button("Cooldown history"); loc.addView(history); history.setOnClickListener(v -> Cooldown.show(activity));
        joystickSwitch = switchRow(loc, "Show joystick"); joystickSwitch.setChecked(Control.joystickVisible());
        joystickSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.joystickVisible())return; host.setJoystickVisible(checked); });
    }

    private void buildCatchSection() {
        // ----- Catch & summary (gated by catch-summary support) -----
        LinearLayout catchC = section(body, "Catch & summary", false); catchRoot = (View)catchC.getParent();
        LinearLayout sumRow = new LinearLayout(activity); sumRow.setOrientation(LinearLayout.HORIZONTAL); sumRow.setGravity(Gravity.CENTER_VERTICAL); sumRow.setMinimumHeight(dp(44));
        quickSummaryLabel = new TextView(activity); quickSummaryLabel.setTextSize(14); sumRow.addView(quickSummaryLabel, new LinearLayout.LayoutParams(0,-2,1f));
        quickSummarySwitch = new Switch(activity); quickSummarySwitch.setContentDescription("Quick catch"); sumRow.addView(quickSummarySwitch, new LinearLayout.LayoutParams(-2,-2)); catchC.addView(sumRow);
        quickSummarySwitch.setChecked(Control.quickSummaryEnabled());
        quickSummarySwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.quickSummaryEnabled())return; Control.setQuickSummaryEnabled(quickSummarySwitch.isChecked()); host.refresh(); });
        quickSummaryStatus = caption(catchC, "");
        LinearLayout txC = section(catchC, "Transfer on catch", false);
        autoTransferSwitch = switchRow(txC, "Transfer below 90% IV"); autoTransferSwitch.setChecked(Control.autoTransferEnabled());
        autoTransferSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.autoTransferEnabled())return; Control.setAutoTransferEnabled(autoTransferSwitch.isChecked()); host.refresh(); });
        String[] protections={"Keep legendary / mythical / Ultra Beasts","Keep shiny Pokemon","Keep event / costume Pokemon","Keep background Pokemon","Keep XXS","Keep XXL"};
        for(int i=0;i<6;i++){final int bit=1<<i;CheckBox box=new CheckBox(activity);box.setText(protections[i]);box.setChecked((Control.transferProtect()&bit)!=0);transferProtection[i]=box;txC.addView(box);
            box.setOnClickListener(v->Control.setTransferProtect(box.isChecked()?Control.transferProtect()|bit:Control.transferProtect()&~bit));}
        transferKeepList = button("Custom keep list"); txC.addView(transferKeepList);
        transferKeepList.setOnClickListener(v -> TransferKeepDialog.show(activity, host::refresh));
        transferOverlaySwitch = switchRow(txC, "Keep-all overlay button"); transferOverlaySwitch.setChecked(Control.transferOverlayEnabled());
        transferOverlaySwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.transferOverlayEnabled())return; Control.setTransferOverlayEnabled(transferOverlaySwitch.isChecked()); host.refresh(); });
        caption(txC, "Kept species are never auto-transferred, across all forms. The floating button holds every catch (Keep All) or follows the rules above.");
        caption(txC, "Transfers are permanent. Favorites and unknown data are always kept.");
        transferStatus = caption(txC, "");
        quickSpinSwitch = switchRow(catchC, "Quick spin"); quickSpinSwitch.setChecked(Control.quickSpinEnabled());
        quickSpinSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.quickSpinEnabled())return; Control.setQuickSpinEnabled(quickSpinSwitch.isChecked()); });
        quickSpinStatus = caption(catchC, "");
        berrySwitch = switchRow(catchC, "Catching berry overlay"); berrySwitch.setChecked(Control.encounterBerryEnabled());
        berrySwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.encounterBerryEnabled())return; Control.setEncounterBerryEnabled(berrySwitch.isChecked()); host.refresh(); });
    }

    private void buildVgpSection() {
        LinearLayout vgpC=section(body,"Virtual Go Plus",false);
        vgpSwitch=switchRow(vgpC,"VGP overlay");vgpSwitch.setChecked(Control.vgpOverlay());
        vgpSwitch.setOnCheckedChangeListener((b,checked)->{if(checked!=Control.vgpOverlay())Control.setVgpOverlay(checked);host.refresh();});
        String[] vgpLabels={"Nearby Pokémon","Nearby PokéStops","Push notifications"};
        for(int i=0;i<3;i++){final int bit=1<<i;CheckBox box=new CheckBox(activity);box.setText(vgpLabels[i]);box.setChecked((Control.vgpOptions()&bit)!=0);vgpChoices[i]=box;vgpC.addView(box);
            box.setOnClickListener(v->{Control.setVgpOptions(box.isChecked()?Control.vgpOptions()|bit:Control.vgpOptions()&~bit);
                if(bit==4&&box.isChecked()&&Build.VERSION.SDK_INT>=33&&activity.checkSelfPermission("android.permission.POST_NOTIFICATIONS")!=android.content.pm.PackageManager.PERMISSION_GRANTED)activity.requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},7301);
            });
        }
        vgpCooldownStop=new CheckBox(activity);vgpCooldownStop.setText("Stop on cooldown (estimated)");vgpCooldownStop.setContentDescription("Stop Virtual Go Plus during the estimated action cooldown");vgpCooldownStop.setChecked(Control.vgpStopOnCooldown());vgpC.addView(vgpCooldownStop);
        vgpCooldownStop.setOnClickListener(v->{if(vgpCooldownStop.isChecked()!=Control.vgpStopOnCooldown())Control.setVgpStopOnCooldown(vgpCooldownStop.isChecked());host.refresh();});
        vgpStatus=caption(vgpC,Control.vgpStatus());
        smartRefresh=SmartCatchSettings.populate(activity,vgpC);
    }

    private void buildIncubationSection() {
        // ----- Auto incubate eggs -----
        LinearLayout eggsC = section(body, "Auto incubate eggs", false);
        incubatorSwitch = switchRow(eggsC, "Auto incubate");
        incubatorSwitch.setChecked(Control.autoIncubate());
        incubatorSwitch.setOnCheckedChangeListener((v, checked) -> {
            if (checked != Control.autoIncubate()) Control.setAutoIncubate(checked);
        });
        incubatorPriority = button("Priority: shortest distance"); eggsC.addView(incubatorPriority);
        incubatorPriority.setOnClickListener(v -> { Control.setIncubateLongest(!Control.incubateLongest()); host.refresh(); });
        caption(eggsC, "Unlimited incubator only");
        incubatorStatus = caption(eggsC, Control.incubatorStatus());
    }

    private void buildBuddySection() {
        LinearLayout buddyC=section(body,"Buddy Emotion Engine",false);buddyRoot=(View)buddyC.getParent();
        buddyRefresh=BuddyEmotionSettings.populate(activity,buddyC);
        buddyStatus=caption(buddyC,"");
    }

    private void buildGiftsSection() {
        // ----- Friend gifts -----
        LinearLayout giftC = section(body, "Friend gifts", false); giftRoot = (View)giftC.getParent();
        String[] giftLabels={"Auto open gifts","Auto send gifts","Add an owned sticker"};
        for(int i=0;i<3;i++){final int bit=1<<i;CheckBox box=new CheckBox(activity);box.setText(giftLabels[i]);box.setChecked((Control.giftOptions()&bit)!=0);giftChoices[i]=box;giftC.addView(box);box.setOnClickListener(v->Control.setGiftOptions(box.isChecked()?Control.giftOptions()|bit:Control.giftOptions()&~bit));}

        giftStatus = caption(giftC, "");
    }

    private void buildItemsSection() {
        // ----- Inventory (opens its modal directly) -----
        LinearLayout itemsC = new LinearLayout(activity); itemsC.setOrientation(LinearLayout.VERTICAL); body.addView(itemsC); itemsRoot = itemsC;
        itemSettings = button("Auto-transfer items"); itemsC.addView(itemSettings); itemSettings.setOnClickListener(v -> showItemSettings());
        itemStatus = caption(itemsC, "");
        Button evolve=button("Batch evolution");itemsC.addView(evolve);
        evolve.setOnClickListener(v -> BatchEvolutionSettings.show(activity));
    }

    private void buildRocketSection() {
        // ----- Team Rocket -----
        LinearLayout rocketC = section(body, "Team Rocket", false); rocketRoot = (View)rocketC.getParent();
        String[] rocketLabels={"Auto quick attack","Auto charged attack","Auto shield"};
        for(int i=0;i<3;i++){final int bit=1<<i;CheckBox box=new CheckBox(activity);box.setText(rocketLabels[i]);box.setChecked((Control.rocketOptions()&bit)!=0);rocketChoices[i]=box;rocketC.addView(box);
            box.setOnClickListener(v->Control.setRocketOptions(box.isChecked()?Control.rocketOptions()|bit:Control.rocketOptions()&~bit));}

        rocketStatus = caption(rocketC, "");
    }

    private void buildBattlesSection() {
        // ----- Raids, gyms & Dynamax -----
        LinearLayout battleC = section(body, "Raids, gyms & Dynamax", false); battleRoot = (View)battleC.getParent();
        String[] battleLabels={"Raids: auto quick attack","Raids: auto charged attack","Gyms: auto quick attack","Gyms: auto charged attack","Dynamax: auto quick attack","Dynamax: auto charged attack","Dynamax: auto Max attack","Raids: auto dodge","Gyms: auto dodge","Dynamax: auto dodge"};
        for(int i=0;i<10;i++){final int bit=1<<i;CheckBox box=new CheckBox(activity);box.setText(battleLabels[i]);box.setChecked((Control.battleOptions()&bit)!=0);battleChoices[i]=box;battleC.addView(box);box.setOnClickListener(v->Control.setBattleOptions(box.isChecked()?Control.battleOptions()|bit:Control.battleOptions()&~bit));}

        battleStatus = caption(battleC, "");
        Button gyms=button("Gym Slotter & Berry Feeder");battleC.addView(gyms);
        gyms.setOnClickListener(v->GymAutomationSettings.show(activity));
    }

    private void buildThrowsSection() {
        // ----- Throws & catch animation (gated by ball-animation support) -----
        LinearLayout throwC = section(body, "Throws", false); quickBallRoot = (View)throwC.getParent();

        throwGrade = button("Throw rating: OFF"); throwC.addView(throwGrade);
        throwGrade.setOnClickListener(v -> { Control.setThrowGrade((Control.throwGrade()+1)%4); host.refresh(); });
        curveSwitch = switchRow(throwC, "Always curveball"); curveSwitch.setChecked(Control.throwCurve());
        curveSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.throwCurve())return; Control.setThrowCurve(curveSwitch.isChecked()); host.refresh(); });
        alwaysHitSwitch = switchRow(throwC, "Always hit"); alwaysHitSwitch.setChecked(Control.throwAlwaysHit());
        alwaysHitSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.throwAlwaysHit())return; Control.setThrowAlwaysHit(alwaysHitSwitch.isChecked()); host.refresh(); });
        arSwitch = switchRow(throwC, "AR+ throw bonus"); arSwitch.setChecked(Control.throwAr());
        arSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.throwAr())return; Control.setThrowAr(arSwitch.isChecked()); host.refresh(); });
        caption(throwC, "Wild encounters. Combines with throw rating and curveball; server bonus unverified.");
        ivCatchSwitch = switchRow(throwC, "Catch IV/level"); ivCatchSwitch.setChecked(Control.ivCatchEnabled());
        ivCatchSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.ivCatchEnabled())return; Control.setIvCatchEnabled(ivCatchSwitch.isChecked()); host.refresh(); });
        ivOwnedSwitch = switchRow(throwC, "Inventory IV/level"); ivOwnedSwitch.setChecked(Control.ivOwnedEnabled());
        ivOwnedSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.ivOwnedEnabled())return; Control.setIvOwnedEnabled(ivOwnedSwitch.isChecked()); host.refresh(); });
        ivStatus = caption(throwC, "");
        quickBallStatus = caption(throwC, "");
    }

    private void buildBackupSection() {
        Button accounts=button("Accounts");body.addView(accounts);
        accounts.setOnClickListener(v->AccountsDialog.show(activity));
        Button settingsBackup=button("Export / import settings");body.addView(settingsBackup);
        settingsBackup.setOnClickListener(v->activity.startActivity(new Intent(activity,LogActivity.class).putExtra("settingsBackup",true)));
    }

    private void buildDiagnosticsSection() {
        // ----- Diagnostics (status plumbing kept out of everyday settings) -----
        LinearLayout diag = section(body, "Diagnostics", false);
        state = caption(diag, Control.connection);
        for(TextView status:new TextView[]{steeringStatus,spawnStatus,quickSummaryStatus,transferStatus,quickSpinStatus,buddyStatus,giftStatus,itemStatus,rocketStatus,battleStatus,quickBallStatus}){
            ((ViewGroup)status.getParent()).removeView(status);diag.addView(status);
        }
        cameraRoot = new LinearLayout(activity); cameraRoot.setOrientation(LinearLayout.VERTICAL); diag.addView(cameraRoot);
        cameraStatus = caption(cameraRoot, ""); cameraStatus.setMaxLines(3); cameraStatus.setEllipsize(android.text.TextUtils.TruncateAt.END);
        cameraSample = button("Sample camera (10s)"); cameraRoot.addView(cameraSample);
        cameraSample.setOnClickListener(v -> { if (Control.requestCameraSample()) { host.releaseJoystick(false); host.refresh(); } });
        interactionRoot = new LinearLayout(activity); interactionRoot.setOrientation(LinearLayout.VERTICAL); diag.addView(interactionRoot);
        interactionStatus = caption(interactionRoot, ""); interactionStatus.setMaxLines(3); interactionStatus.setEllipsize(android.text.TextUtils.TruncateAt.END);
        interactionCapture = button("Capture catch/spin (60s)"); interactionRoot.addView(interactionCapture);
        interactionCapture.setOnClickListener(v -> {
            if (Control.interactionDiagnosticRunning()) Control.requestInteractionStop();
            else if (Control.requestInteractionTrace()) host.releaseJoystick(false);
            host.refresh();
        });
        Button logs = button("Run log / export"); diag.addView(logs);
        logs.setOnClickListener(v -> activity.startActivity(new Intent(activity, LogActivity.class)));
    }

    /** The root panel view; Pad owns attachment to the window and insets wiring. */
    View view(){return panel;}
    boolean isVisible(){return panel.getVisibility()==View.VISIBLE;}
    /** Bring the full-screen panel up; Pad applies current insets immediately after. */
    void show(){panel.setVisibility(View.VISIBLE);panel.bringToFront();panel.requestApplyInsets();}
    /** Hidden state matches the original startup and the close affix: returns to the game underneath. */
    void hide(){panel.setVisibility(View.GONE);}
    /** Release this panel's dialog polling when its owning activity panel is detached. */
    void close(){
        for(AlertDialog dialog:new java.util.ArrayList<>(itemDialogs))dialog.dismiss();
        itemDialogs.clear();handler.removeCallbacksAndMessages(null);
    }

    /** Pads the full-screen settings panel inside the status bar, navigation bar, and IME. */
    void applyInsets(WindowInsets insets) {
        if(insets==null)return;
        int top,bottom,left,right;
        if(Build.VERSION.SDK_INT>=30){
            android.graphics.Insets bars=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.ime());
            android.graphics.Insets safe=insets.getInsetsIgnoringVisibility(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout());
            top=Math.max(bars.top,safe.top);bottom=Math.max(bars.bottom,safe.bottom);left=Math.max(bars.left,safe.left);right=Math.max(bars.right,safe.right);
        }else{
            top=insets.getSystemWindowInsetTop();bottom=insets.getSystemWindowInsetBottom();
            left=insets.getSystemWindowInsetLeft();right=insets.getSystemWindowInsetRight();
        }
        if(top==0){int id=activity.getResources().getIdentifier("status_bar_height","dimen","android");top=id!=0?activity.getResources().getDimensionPixelSize(id):dp(28);}
        panel.setPadding(dp(12)+left,dp(8)+top,dp(12)+right,dp(12)+bottom);
    }

    void applyTheme(){
        panel.setBackgroundColor(surface());styleTree(panel);
        for(TextView head:sectionHeads){head.setTextColor(accent());head.setTypeface(Typeface.DEFAULT,Typeface.BOLD);}
        for(TextView c:captions)c.setTextColor(muted());
        if(coords!=null)coords.setTextColor(ink());
        if(closeIcon!=null)closeIcon.invalidate();
        if(themeIcon!=null){themeIcon.setContentDescription(Control.darkTheme()?"Switch to light theme":"Switch to dark theme");themeIcon.invalidate();}
    }

    private void showItemSettings(){
        Control.requestItemCatalog();
        LinearLayout form=new LinearLayout(activity);form.setOrientation(LinearLayout.VERTICAL);form.setPadding(dp(16),dp(8),dp(16),dp(8));
        CheckBox enabled=new CheckBox(activity);enabled.setText("Enable automatic item transfer");enabled.setChecked(Control.itemTransferEnabled());form.addView(enabled);
        TextView explanation=new TextView(activity);explanation.setText("Check items to trim and set the maximum to keep. Saving applies to your current bag and future items. A limit of 0 removes all of that item. Unchecked items are kept.");form.addView(explanation);
        java.util.Map<Integer,Integer> limits=new java.util.HashMap<>();java.util.Set<Integer> selected=new java.util.HashSet<>();
        for(String row:Control.itemLimits().split(";"))try{String[] b=row.split(":");limits.put(Integer.parseInt(b[0]),Integer.parseInt(b[1]));}catch(RuntimeException ignored){}
        for(String row:Control.itemRules().split(";"))try{String[] b=row.split(":");int id=Integer.parseInt(b[0]);selected.add(id);limits.put(id,Integer.parseInt(b[1]));}catch(RuntimeException ignored){}
        LinearLayout rows=new LinearLayout(activity);rows.setOrientation(LinearLayout.VERTICAL);ScrollView scroll=new ScrollView(activity);scroll.addView(rows);form.addView(scroll,new LinearLayout.LayoutParams(-1,dp(380)));
        java.util.Map<Integer,CheckBox> boxes=new java.util.LinkedHashMap<>();java.util.Map<Integer,EditText> inputs=new java.util.LinkedHashMap<>();
        Runnable populate=()->{for(String row:Control.itemCatalog().split("\n")){String[] b=row.split("\\|",-1);if(b.length!=3)continue;int id;try{id=Integer.parseInt(b[0]);}catch(NumberFormatException invalid){continue;}if(boxes.containsKey(id)){boxes.get(id).setText(b[1]+" ("+b[2]+" in bag)");continue;}
            LinearLayout line=new LinearLayout(activity);line.setOrientation(LinearLayout.HORIZONTAL);line.setGravity(Gravity.CENTER_VERTICAL);
            CheckBox box=new CheckBox(activity);box.setText(b[1]+" ("+b[2]+" in bag)");box.setChecked(selected.contains(id));line.addView(box,new LinearLayout.LayoutParams(0,-2,1));
            EditText cap=new EditText(dialogContext());cap.setInputType(InputType.TYPE_CLASS_NUMBER);cap.setSingleLine(true);cap.setHint("Max");cap.setText(String.valueOf(limits.containsKey(id)?limits.get(id):100));cap.setContentDescription("Maximum "+b[1]+" to keep");SettingsUi.enabled(cap,enabled.isChecked()&&box.isChecked());box.setOnCheckedChangeListener((button,checked)->SettingsUi.enabled(cap,enabled.isChecked()&&checked));line.addView(cap,new LinearLayout.LayoutParams(dp(80),-2));rows.addView(line);styleTree(line);boxes.put(id,box);inputs.put(id,cap);
        }};populate.run();
        Runnable dependencies=()->{for(java.util.Map.Entry<Integer,CheckBox> entry:boxes.entrySet()){SettingsUi.enabled(entry.getValue(),enabled.isChecked());SettingsUi.enabled(inputs.get(entry.getKey()),enabled.isChecked()&&entry.getValue().isChecked());}};
        enabled.setOnCheckedChangeListener((v,on)->dependencies.run());dependencies.run();
        TextView status=new TextView(activity);status.setText(Control.itemStatus());form.addView(status);
        AlertDialog dialog=new AlertDialog.Builder(dialogContext()).setTitle("Auto-transfer inventory items").setView(form).setNegativeButton("Cancel",null).setPositiveButton("Save & apply",null).create();
        final Runnable[] poll=new Runnable[1];poll[0]=()->{if(!dialog.isShowing())return;Control.requestItemCatalog();populate.run();dependencies.run();status.setText(Control.itemStatus());handler.postDelayed(poll[0],1000);};
        itemDialogs.add(dialog);
        dialog.setOnDismissListener(d->{handler.removeCallbacks(poll[0]);itemDialogs.remove(dialog);});dialog.show();styleTree(form);handler.post(poll[0]);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{StringBuilder rules=new StringBuilder(),all=new StringBuilder();for(java.util.Map.Entry<Integer,EditText> e:inputs.entrySet()){int cap;try{cap=Integer.parseInt(e.getValue().getText().toString().trim());if(cap<0||cap>100000)throw new NumberFormatException();}catch(NumberFormatException invalid){e.getValue().setError("Enter 0 to 100000");return;}String rule=e.getKey()+":"+cap+";";all.append(rule);if(boxes.get(e.getKey()).isChecked())rules.append(rule);}
            Control.saveItemRules(enabled.isChecked(),rules.toString(),all.toString());dialog.dismiss();host.refresh();});
    }

    void refresh() {
        refreshLocationStatus();
        refreshDiagnostics();
        refreshSummaryVisibility();
        refreshTransfer();
        refreshVgpStatus();
        refreshIncubation();
        refreshVgpOptions();
        refreshBuddy();
        refreshGifts();
        refreshBattles();
        refreshRocket();
        refreshItems();
        refreshQuickCatch();
        refreshLocationOptions();
        refreshThrows();
        refreshDependencies();
    }

    private void refreshDependencies(){
        for(CheckBox box:vgpChoices)SettingsUi.enabled(box,Control.vgpOverlay());
        SettingsUi.enabled(vgpCooldownStop,Control.vgpOverlay());smartRefresh.run();
        for(CheckBox box:transferProtection)SettingsUi.enabled(box,Control.autoTransferEnabled());
        SettingsUi.enabled(transferKeepList,Control.autoTransferEnabled());
        SettingsUi.enabled((View)transferOverlaySwitch.getParent(),Control.autoTransferEnabled());
        SettingsUi.enabled(incubatorPriority,Control.autoIncubate()&&Control.incubatorAvailable());
        SettingsUi.enabled(giftChoices[2],(Control.giftOptions()&2)!=0);
        SettingsUi.enabled((View)joystickSwitch.getParent(),Control.desiredEnabled());
        SettingsUi.enabled(mapTap,Control.desiredEnabled()&&Control.quickBallConfigured());
    }

    private void refreshLocationStatus() {
        if(uiScale!=null)uiScale.setText("UI scale: "+Control.uiScalePercent()+"%");
        coords.setText((Control.desiredEnabled()?"":"Saved spoof location\n")+String.format(Locale.US, "%.6f, %.6f\n%.1f km/h", Control.engine.latitude, Control.engine.longitude, Control.engine.speedKmh));
        MotionEngine.RouteProgress route=Control.engine.routeProgress();
        locationSwitch.setChecked(Control.desiredEnabled());
        String movement=!Control.desiredEnabled()?"DEVICE LOCATION":Control.engine.enabled ? (Control.engine.walking ? "WALKING" : "ON") : "WAITING FOR ADAPTER";
        if (route.total>1 && route.active) movement=String.format(Locale.US,"ROUTE %d/%d  ·  %.0f m left",route.visited+1,route.total,route.remainingMeters);
        else if (route.total>1 && route.visited==route.total) movement="ON  ·  route complete";
        state.setText(Control.connection + "\n" + movement + "  ·  coordinate reads " + Control.latitudeReads + "/" + Control.longitudeReads);
        steeringStatus.setText(Control.cameraSteeringConfigured() ? Control.cameraSteeringStatus() : "Joystick follows the camera");
    }

    private void refreshDiagnostics() {
        cameraRoot.setVisibility(Control.cameraDiagnosticsConfigured() ? View.VISIBLE : View.GONE);
        cameraStatus.setText(Control.cameraDiagnosticStatus());
        cameraSample.setText(Control.cameraDiagnosticRunning() ? "Sampling camera..." : "Sample camera (10s)");
        cameraSample.setEnabled(Control.cameraDiagnosticsReady() && !Control.cameraSamplePending());
        interactionRoot.setVisibility(Control.interactionDiagnosticsConfigured() ? View.VISIBLE : View.GONE);
        interactionStatus.setText(Control.interactionDiagnosticStatus());
        interactionCapture.setText(Control.interactionDiagnosticRunning() ? "Stop capture" : "Capture catch/spin (60s)");
        interactionCapture.setEnabled(Control.interactionDiagnosticRunning() || (Control.interactionDiagnosticsReady() && !Control.interactionTracePending()));
    }

    private void refreshSummaryVisibility() {
        int summaryGate = Control.quickSummaryConfigured() ? View.VISIBLE : View.GONE;
        catchRoot.setVisibility(summaryGate); buddyRoot.setVisibility(summaryGate); giftRoot.setVisibility(summaryGate);
        itemsRoot.setVisibility(summaryGate); rocketRoot.setVisibility(summaryGate); battleRoot.setVisibility(summaryGate);
    }

    private void refreshTransfer() {
        quickSummaryStatus.setText(Control.quickSummaryStatus());
        autoTransferSwitch.setChecked(Control.autoTransferEnabled());
        for(int i=0;i<6;i++)transferProtection[i].setChecked((Control.transferProtect()&(1<<i))!=0);
        if(transferKeepList!=null){String kept=Control.transferKeepSpecies();int n=kept.isEmpty()?0:kept.split(",").length;transferKeepList.setText(n>0?"Custom keep list ("+n+")":"Custom keep list");}
        if(transferOverlaySwitch!=null){transferOverlaySwitch.setChecked(Control.transferOverlayEnabled());transferOverlaySwitch.setEnabled(Control.autoTransferEnabled());transferOverlaySwitch.setAlpha(1f);}
        transferStatus.setText(Control.transferStatus());
    }

    private void refreshVgpStatus() {
        vgpSwitch.setChecked(Control.vgpOverlay());vgpStatus.setText(Control.vgpStatus());
    }

    private void refreshIncubation() {
        incubatorSwitch.setChecked(Control.autoIncubate());
        incubatorSwitch.setEnabled(Control.incubatorAvailable());
        incubatorPriority.setText("Priority: " + (Control.incubateLongest() ? "longest" : "shortest") + " distance");
        incubatorStatus.setText(Control.autoIncubate() ? Control.incubatorStatus() : "Off");
    }

    private void refreshVgpOptions() {
        for(int i=0;i<3;i++)vgpChoices[i].setChecked((Control.vgpOptions()&(1<<i))!=0);
        vgpCooldownStop.setChecked(Control.vgpStopOnCooldown());
    }

    private void refreshBuddy() {buddyRefresh.run();buddyStatus.setText(BuddyEmotionSettings.status());}

    private void refreshGifts() {
        for(int i=0;i<3;i++)giftChoices[i].setChecked((Control.giftOptions()&(1<<i))!=0);giftStatus.setText(Control.giftStatus());
    }

    private void refreshBattles() {
        for(int i=0;i<10;i++)battleChoices[i].setChecked((Control.battleOptions()&(1<<i))!=0);battleStatus.setText(Control.battleStatus());
    }

    private void refreshRocket() {
        for(int i=0;i<3;i++)rocketChoices[i].setChecked((Control.rocketOptions()&(1<<i))!=0);rocketStatus.setText(Control.rocketStatus());
    }

    private void refreshItems() {
        itemSettings.setText("Auto-transfer items: "+(Control.itemTransferEnabled()?"ON":"OFF"));itemStatus.setText(Control.itemStatus());
    }

    private void refreshQuickCatch() {
        quickSpinSwitch.setChecked(Control.quickSpinEnabled());quickSpinStatus.setText(Control.quickSpinStatus());
        quickSummaryLabel.setText(Control.quickSummaryAutoDismiss() ? "Quick catch" : "Faster catch summary");
        quickSummarySwitch.setChecked(Control.quickSummaryEnabled());
        quickSummarySwitch.setEnabled(Control.quickSummaryConfigured());
        quickBallRoot.setVisibility(Control.quickBallConfigured() ? View.VISIBLE : View.GONE);
        quickBallStatus.setText(Control.quickBallStatus());
    }

    private void refreshLocationOptions() {
        mapTap.setText("Game map tap: "+new String[]{"Do nothing","Walk","Teleport","Walk / double tap teleport"}[Control.mapTapMode()]);
        mapTap.setEnabled(Control.quickBallConfigured());
        spawnSwitch.setChecked(Control.spawnBooster());spawnSwitch.setEnabled(Control.spawnAllowed());spawnStatus.setText(Control.spawnStatus());
    }

    private void refreshThrows() {
        throwGrade.setText("Throw rating: "+new String[]{"OFF","Nice","Great","Excellent"}[Control.throwGrade()]);
        throwGrade.setEnabled(Control.quickBallConfigured());
        curveSwitch.setChecked(Control.throwCurve());curveSwitch.setEnabled(Control.quickBallConfigured());
        alwaysHitSwitch.setChecked(Control.throwAlwaysHit());alwaysHitSwitch.setEnabled(Control.throwAlwaysHitReady() || Control.throwAlwaysHit());
        arSwitch.setChecked(Control.throwAr());arSwitch.setEnabled(Control.throwArReady());
        ivCatchSwitch.setChecked(Control.ivCatchEnabled());ivCatchSwitch.setEnabled(Control.quickBallConfigured());
        ivOwnedSwitch.setChecked(Control.ivOwnedEnabled());ivOwnedSwitch.setEnabled(Control.quickBallConfigured());
        ivStatus.setText(Control.ivDisplayStatus());
    }
}
