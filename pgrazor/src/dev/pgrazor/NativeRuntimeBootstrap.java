package dev.pgrazor;
import java.util.concurrent.atomic.AtomicInteger;
/** Called by the compiled DEX bridge after nativeRender returns on Unity's thread. */
public final class NativeRuntimeBootstrap {
    // 0 unarmed,1 armed,2 terminal,3 in-flight. Reentry never dispatches twice.
    private static final AtomicInteger state=new AtomicInteger();
    private static final AtomicInteger gameplay=new AtomicInteger();
    private static volatile boolean bootstrapReady;
    private NativeRuntimeBootstrap() {}
    public static void start(){state.compareAndSet(0,1);}
    /** Only the compiled-only package path may attest exclusive gameplay ownership. */
    public static void startGameplay(){start();gameplay.compareAndSet(0,1);}
    public static void onRendered(){
        if(!state.compareAndSet(1,3)){if(bootstrapReady)startGameplayOnOwner();return;}
        try{
            // Native requires an already-attached IL2CPP caller. A domain pointer
            // alone is not initialization proof; no background thread_attach.
            long packed=NativeCore.bootstrapProbe();int status=(int)(packed>>>32),detail=(int)packed;
            if(status==1||status==-5){state.set(1);return;}
            state.set(2);
            if(status==0){bootstrapReady=true;Control.event("native-bootstrap-ready","Compiled IL2CPP binding on rendered Unity thread");}
            else Control.event("native-bootstrap-unavailable","status="+status+" detail="+detail);
        }catch(Throwable failure){
            state.set(2);
            try{Control.event("native-bootstrap-unavailable",failure.getClass().getSimpleName());}catch(Throwable ignored){}
        }
        if(bootstrapReady)startGameplayOnOwner();
    }
    private static void startGameplayOnOwner(){
        if(!gameplay.compareAndSet(1,3))return;
        try{
            int status=NativeCore.runtimeStart(1);
            if(status==1){gameplay.set(1);return;}
            gameplay.set(2);
            Control.event(status==0?"native-scheduler-ready":"native-scheduler-unavailable",
                status==0?"Compiled scheduler installed; individual gameplay hosts initialize separately":"status="+status);
        }catch(Throwable failure){
            gameplay.set(2);
            try{Control.event("native-scheduler-unavailable",failure.getClass().getSimpleName());}catch(Throwable ignored){}
        }
    }
}
