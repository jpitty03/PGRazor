package dev.pgrazor;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import com.nianticlabs.nia.contextservice.ContextService;
import com.nianticlabs.nia.location.NianticLocationManager;
import com.nianticlabs.platform.authgooglesignin.SignInResponse;
import com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus;
import com.nianticlabs.background.configuration.InitialDialog;
import java.util.ArrayList;
import java.util.List;

public final class GameBridgeDispatchTest {
    public static final class Consumer implements kotlin.jvm.functions.Function1 {
        final List<SignInResponse> values=new ArrayList<>();
        public Object invoke(Object value){values.add((SignInResponse)value);return null;}
    }
    private static void check(boolean value){if(!value)throw new AssertionError("dispatch fixture assertion");}
    private static void noSensitiveLog(){for(String line:Control.events){check(!line.contains("fixture-token")&&!line.contains("fixture-sensitive-failure"));}}
    private static void basic() throws Exception {
        Class.forName("dev.pgrazor.GameBridgeDispatch");
        check(Control.events.isEmpty()&&ContextService.factoryCalls==0&&GoogleBrowserAuth.beginCount==0);
        GameBridgeDispatch.beforePause(new Object());GameBridgeDispatch.beforeStop(null);
        check(ContextService.factoryCalls==0&&Control.events.isEmpty());
        check(GameBridgeDispatch.locationPermission("android.permission.ACCESS_FINE_LOCATION"));
        check(!GameBridgeDispatch.locationPermission("android.permission.CAMERA"));
        check(GameBridgeDispatch.permissionStatus(1)==PermissionStatus.GRANTED_IN_USE);
        check(GameBridgeDispatch.permissionStatus(9007199254740993L)==PermissionStatus.GRANTED_ALWAYS);
        Control.desired=false;
        check(!GameBridgeDispatch.locationPermission("android.permission.ACCESS_FINE_LOCATION"));
        check(GameBridgeDispatch.permissionStatus(1)==null);Control.desired=true;
        InitialDialog dialog=new InitialDialog();GameBridgeDispatch.dismissDialog(dialog,"onClickCancel");check(dialog.cancelled);
        InitialDialog untouched=new InitialDialog();GameBridgeDispatch.dismissDialog(untouched,"onClickClose");check(!untouched.cancelled);
        GameBridgeDispatch.dismissDialog(new Object(),"onClickCancel");

        NianticLocationManager manager=new NianticLocationManager();
        GameBridgeDispatch.afterStart(manager);check(ContextService.factoryCalls==1);
        ContextService.handler.runNext();check(manager.delivered.size()==1);
        GameBridgeDispatch.beforePause(manager);check(ContextService.handler.pending.isEmpty());
        GameBridgeDispatch.afterResume(manager);ContextService.handler.runNext();check(manager.delivered.size()==2);
        GameBridgeDispatch.beforeStop(manager);check(ContextService.handler.pending.isEmpty());

        Consumer success=new Consumer();GameBridgeDispatch.signIn(new Activity(),success);
        check(GoogleBrowserAuth.beginCount==0&&success.values.isEmpty()); // always queued
        Handler.runGlobalNext();check(GoogleBrowserAuth.beginCount==1);
        // Authentication remains alive across the game's pause/background gates.
        Control.active=false;GameBridgeDispatch.beforePause(manager);
        GoogleBrowserAuth.results.add("{\"status\":\"Success\",\"email\":\"fixture@example.invalid\",\"idToken\":\"fixture-token\"}");
        Handler.runGlobalNext();check(success.values.size()==1&&success.values.get(0).status.equals("Success"));
        check(success.values.get(0).idToken.equals("fixture-token"));Control.active=true;

        Consumer pending=new Consumer();GameBridgeDispatch.signIn(new Activity(),pending);Handler.runGlobalNext();
        GameBridgeDispatch.shutdown();GameBridgeDispatch.shutdown();Handler.drain(5);
        check(pending.values.size()==1&&pending.values.get(0).status.equals("Cancelled"));
        int begins=GoogleBrowserAuth.beginCount;
        Consumer late=new Consumer();GameBridgeDispatch.signIn(new Activity(),late);Handler.drain(3);
        check(late.values.size()==1&&late.values.get(0).status.equals("Cancelled")&&GoogleBrowserAuth.beginCount==begins);
        check(!GameBridgeDispatch.locationPermission("android.permission.ACCESS_FINE_LOCATION")&&GameBridgeDispatch.permissionStatus(1)==null);
        GameBridgeDispatch.afterResume(manager);check(ContextService.handler.pending.isEmpty()&&ContextService.factoryCalls==1);
        InitialDialog stopped=new InitialDialog();GameBridgeDispatch.dismissDialog(stopped,"onClickCancel");check(!stopped.cancelled);
    }
    private static void isolation(){
        NianticLocationManager manager=new NianticLocationManager();
        Looper.setMain(false);GameBridgeDispatch.afterStart(manager);Looper.setMain(true);
        check(ContextService.factoryCalls==0); // main-thread requirement isolated, not bypassed
        ContextService.factoryFails=true;GameBridgeDispatch.afterStart(manager);
        check(ContextService.factoryCalls==1&&ContextService.handler.pending.isEmpty());
        check(GameBridgeDispatch.locationPermission("android.permission.ACCESS_COARSE_LOCATION"));
        Consumer consumer=new Consumer();GameBridgeDispatch.signIn(new Activity(),consumer);Handler.runGlobalNext();
        check(GoogleBrowserAuth.beginCount==1);
        ContextService.factoryFails=false;GameBridgeDispatch.afterStart(manager);check(ContextService.factoryCalls==2);
        ContextService.handler.runNext();check(manager.delivered.size()==1);
        GameBridgeDispatch.shutdown();Handler.drain(8);check(consumer.values.size()==1);
    }
    private static void shutdownDuringCreate(){
        Control.onEvent=GameBridgeDispatch::shutdown;
        boolean failed=false;
        try{GameBridgeDispatch.signIn(new Activity(),new Consumer());}catch(IllegalStateException expected){failed=true;}
        check(failed);Handler.drain(4);check(GoogleBrowserAuth.beginCount==0);
        check(!GameBridgeDispatch.locationPermission("android.permission.ACCESS_FINE_LOCATION"));
        GameBridgeDispatch.afterStart(new NianticLocationManager());check(ContextService.factoryCalls==0);
    }
    private static void queueFailure(){
        Handler.accept=false;Looper.setMain(false);boolean failed=false;
        try{GameBridgeDispatch.signIn(new Activity(),new Consumer());}catch(IllegalStateException expected){
            failed=expected.getMessage().equals("Game sign-in main-thread queue unavailable");
        }
        check(failed&&GoogleBrowserAuth.beginCount==0);Looper.setMain(true);
        Consumer mainFailure=new Consumer();GameBridgeDispatch.signIn(new Activity(),mainFailure);
        check(mainFailure.values.size()==1&&mainFailure.values.get(0).status.equals("Fail"));
        Handler.accept=true;GameBridgeDispatch.shutdown();Handler.drain(3);
    }
    private static void inertShutdown(){
        GameBridgeDispatch.shutdown();check(Control.events.isEmpty());
        boolean failed=false;try{GameBridgeDispatch.signIn(new Activity(),new Consumer());}catch(IllegalStateException expected){failed=true;}
        check(failed&&GoogleBrowserAuth.beginCount==0);
        check(!GameBridgeDispatch.locationPermission("android.permission.ACCESS_FINE_LOCATION"));
        GameBridgeDispatch.afterStart(new NianticLocationManager());check(ContextService.factoryCalls==0);
    }
    private static void concurrentCreate() throws Exception {
        Consumer first=new Consumer(),second=new Consumer();
        Control.onEvent=()->{try{Thread.sleep(40);}catch(InterruptedException e){Thread.currentThread().interrupt();}};
        List<Throwable> failures=java.util.Collections.synchronizedList(new ArrayList<>());
        Thread a=new Thread(()->{try{GameBridgeDispatch.signIn(new Activity(),first);}catch(Throwable failure){failures.add(failure);}});
        Thread b=new Thread(()->{try{GameBridgeDispatch.signIn(new Activity(),second);}catch(Throwable failure){failures.add(failure);}});
        a.start();b.start();a.join();b.join();check(failures.isEmpty());
        long installed=Control.events.stream().filter(s->s.equals("google-browser|Browser sign-in adapter installed")).count();check(installed==1);
        Handler.runGlobalNext();Handler.runGlobalNext();check(GoogleBrowserAuth.beginCount==2);
        GameBridgeDispatch.shutdown();Handler.drain(5);check(first.values.size()==1&&second.values.size()==1);
    }
    private static void reentrantCreate(){
        boolean[] rejected={false};Consumer nested=new Consumer(),outer=new Consumer();
        Control.onEvent=()->{try{GameBridgeDispatch.signIn(new Activity(),nested);}catch(IllegalStateException expected){rejected[0]=true;}};
        GameBridgeDispatch.signIn(new Activity(),outer);check(rejected[0]&&nested.values.isEmpty());
        Handler.runGlobalNext();check(GoogleBrowserAuth.beginCount==1);
        GameBridgeDispatch.shutdown();Handler.drain(5);check(outer.values.size()==1);
    }
    private static void shutdownCallback(){
        NianticLocationManager manager=new NianticLocationManager();
        com.nianticlabs.nia.location.NianticLocationProvider.ProviderListener listener=new com.nianticlabs.nia.location.NianticLocationProvider.ProviderListener(){
            public void onProviderStatus(int status){}public void onProviderLocation(android.location.Location fix){}
            public void onProviderSatellites(long time,int count){}
        };
        com.nianticlabs.nia.location.NianticLocationProvider provider=new com.nianticlabs.nia.location.NianticLocationProvider("fixture",listener);
        manager.providers.put("fixture",provider);GameBridgeDispatch.afterStart(manager);ContextService.handler.runNext();
        Consumer pending=new Consumer(),late=new Consumer();GameBridgeDispatch.signIn(new Activity(),pending);Handler.runGlobalNext();
        check(GoogleBrowserAuth.beginCount==1);
        provider.onSetListener=()->{GameBridgeDispatch.signIn(new Activity(),late);Handler.runGlobalNext();Handler.runGlobalNext();};
        GameBridgeDispatch.shutdown();Handler.drain(8);
        check(GoogleBrowserAuth.beginCount==1&&pending.values.size()==1&&late.values.size()==1);
        check(late.values.get(0).status.equals("Cancelled"));
    }
    private static void preparedServiceThread() throws Exception {
        NianticLocationManager manager=new NianticLocationManager();
        List<Throwable> failures=java.util.Collections.synchronizedList(new ArrayList<>());
        Thread first=new Thread(()->{
            Looper.setMain(false);boolean refused=false;
            try{GameBridgeDispatch.prepareDelivery();}catch(IllegalStateException expected){refused=true;}
            if(!refused)failures.add(new AssertionError("first off-main preparation accepted"));
            // An unprepared lifecycle callback must isolate the same factory failure.
            GameBridgeDispatch.afterStart(manager);
        });first.start();first.join();check(failures.isEmpty()&&ContextService.factoryCalls==0);
        ContextService.factoryFails=true;boolean factoryFailure=false;
        try{GameBridgeDispatch.prepareDelivery();}catch(IllegalStateException expected){factoryFailure=true;}
        check(factoryFailure&&ContextService.factoryCalls==1&&ContextService.handler.pending.isEmpty());
        ContextService.factoryFails=false;GameBridgeDispatch.prepareDelivery();GameBridgeDispatch.prepareDelivery();
        check(ContextService.factoryCalls==2&&ContextService.handler.pending.isEmpty());
        Thread service=new Thread(()->{
            Looper.setMain(false);
            try{
                GameBridgeDispatch.afterStart(manager);ContextService.handler.runNext();
                check(manager.delivered.size()==1);
                GameBridgeDispatch.beforePause(manager);check(ContextService.handler.pending.isEmpty());
                GameBridgeDispatch.afterResume(manager);ContextService.handler.runNext();check(manager.delivered.size()==2);
                GameBridgeDispatch.beforeStop(manager);check(ContextService.handler.pending.isEmpty());
            }catch(Throwable failure){failures.add(failure);}
        });service.start();service.join();check(failures.isEmpty()&&ContextService.factoryCalls==2);
        GameBridgeDispatch.shutdown();boolean refused=false;
        try{GameBridgeDispatch.prepareDelivery();}catch(IllegalStateException expected){refused=true;}
        check(refused&&ContextService.factoryCalls==2&&ContextService.handler.pending.isEmpty());
    }
    private static void browserOption(){
        check(!GameBridgeDispatch.browserSignInEnabled());
        GameBridgeDispatch.configureBrowserSignIn(true);check(GameBridgeDispatch.browserSignInEnabled());
        GameBridgeDispatch.configureBrowserSignIn(false);check(!GameBridgeDispatch.browserSignInEnabled());
        GameBridgeDispatch.configureBrowserSignIn(true);check(GameBridgeDispatch.browserSignInEnabled());
        check(GoogleBrowserAuth.beginCount==0&&Control.events.isEmpty()&&ContextService.factoryCalls==0);
        // Configuring the wrapper gate does not construct any family or cancel old
        // requests. The DEX wrapper owns the original-body branch while disabled.
        Consumer consumer=new Consumer();GameBridgeDispatch.signIn(new Activity(),consumer);Handler.runGlobalNext();
        check(GoogleBrowserAuth.beginCount==1);
        GameBridgeDispatch.shutdown();Handler.drain(4);check(!GameBridgeDispatch.browserSignInEnabled());
        GameBridgeDispatch.configureBrowserSignIn(true);check(!GameBridgeDispatch.browserSignInEnabled());
        GameBridgeDispatch.configureBrowserSignIn(false);check(!GameBridgeDispatch.browserSignInEnabled());
        int begins=GoogleBrowserAuth.beginCount;Consumer late=new Consumer();GameBridgeDispatch.signIn(new Activity(),late);Handler.drain(3);
        check(GoogleBrowserAuth.beginCount==begins&&late.values.size()==1&&late.values.get(0).status.equals("Cancelled"));
    }
    public static void main(String[] args) throws Exception {
        check(args.length==1);
        switch(args[0]){
            case "basic":basic();break;case "isolation":isolation();break;
            case "shutdown-during-create":shutdownDuringCreate();break;case "queue-failure":queueFailure();break;
            case "inert-shutdown":inertShutdown();break;case "concurrent-create":concurrentCreate();break;
            case "reentrant-create":reentrantCreate();break;case "shutdown-callback":shutdownCallback();break;
            case "prepared-service-thread":preparedServiceThread();break;case "browser-option":browserOption();break;default:throw new AssertionError("unknown fixture");
        }
        noSensitiveLog();System.out.println("PASS GameBridgeDispatch "+args[0]);
    }
}
