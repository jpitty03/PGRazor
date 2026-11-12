package dev.pgrazor;
import java.util.concurrent.atomic.AtomicInteger;
/** Called by the compiled DEX bridge after nativeRender returns on Unity's thread. */
public final class NativeRuntimeBootstrap {
    // 0 unarmed,1 armed,2 terminal,3 in-flight. Reentry never dispatches twice.
    private static final AtomicInteger state=new AtomicInteger();
    private static final AtomicInteger gameplay=new AtomicInteger();
    private static volatile boolean bootstrapReady;
    // Pending-probe diagnostics. The rendered Unity thread re-probes every frame while an
    // attached IL2CPP caller is not yet proven, and the scheduler re-arms every frame while it
    // reports "not ready"; both loops are otherwise silent. A single bounded notice per loop
    // marks a prolonged wait, so a user report that stays stuck with the overlay still up can be
    // told apart from the normal few-frame startup delay. Counters are only read/written inside
    // the CAS-exclusive in-flight section (state==3 / gameplay==3), so the AtomicInteger acquire
    // on the next entry publishes the previous frame's increment; plain ints are safe here.
    static final int BOOTSTRAP_PENDING_PROBES=300;
    static final int SCHEDULER_PENDING_PROBES=300;
    private static int bootstrapProbes;
    private static int schedulerProbes;
    /** Diagnostics must never alter the state machine, so an event sink failure is swallowed. */
    private static void note(String kind,String detail){try{Control.event(kind,detail);}catch(Throwable ignored){}}
    private static String bootstrapReason(int status,int detail){
        switch(status){
            case 1:return loaderReason(detail);case -1:return "null-argument";
            case -2:return "abi-version-mismatch";case -3:return "abi-size-mismatch";
            case -4:return "invalid-configuration";case -5:return "concurrent-probe";
            case -6:return "missing-il2cpp-export";case -7:return "invalid-module-range";
            case -8:return "metadata-api-unavailable";case -9:return "metadata-bind-failed";
            case -10:return "execute-tasks-resolution-failed";case -11:return "thread-attach-failed";
            case -12:return "poisoned";case -13:return "export-code-validation-failed";
            default:return "unknown-status";
        }
    }
    // PGR_BOOT_NOT_READY detail -> PgrBootLoaderDetail (runtime_bootstrap.h). The rooted bootstrap
    // resolves Android's private __loader_dlopen via RTLD_DEFAULT and, on an isolated payload
    // namespace, through the already-loaded libdl/ld-android handle; these localize a stuck wait.
    // Unknown/0 details (including the ordinary library/domain/thread wait) stay the plain pending.
    private static String loaderReason(int detail){
        switch(detail){
            case 1101:return "android-loader-image-ambiguous";
            case 1102:return "android-loader-no-executable-segment";
            case 1103:return "android-loader-dlopen-symbol-unavailable";
            case 1104:return "android-loader-namespace-open-null";
            default:return "library-thread-or-domain-pending";
        }
    }
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
            if(status==1||status==-5){
                // One bounded notice once the wait is clearly past a transient startup delay.
                if(++bootstrapProbes==BOOTSTRAP_PENDING_PROBES)
                    note("native-bootstrap-pending","status="+status+" detail="+detail+" attempts="+bootstrapProbes+" reason="+bootstrapReason(status,detail));
                state.set(1);return;
            }
            state.set(2);
            if(status==0){bootstrapReady=true;Control.event("native-bootstrap-ready","Compiled IL2CPP binding on rendered Unity thread");}
            else Control.event("native-bootstrap-unavailable","status="+status+" detail="+detail+" reason="+bootstrapReason(status,detail));
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
            if(status==1){
                // The bootstrap binding is ready but the scheduler keeps reporting "not ready";
                // one bounded notice distinguishes a stuck scheduler from a brief retry.
                if(++schedulerProbes==SCHEDULER_PENDING_PROBES)
                    note("native-scheduler-pending","status=1 attempts="+schedulerProbes);
                gameplay.set(1);return;
            }
            gameplay.set(2);
            Control.event(status==0?"native-scheduler-ready":"native-scheduler-unavailable",
                status==0?"Compiled scheduler installed; individual gameplay hosts initialize separately":"status="+status+" detailEvent=native-runtime-start");
        }catch(Throwable failure){
            gameplay.set(2);
            try{Control.event("native-scheduler-unavailable",failure.getClass().getSimpleName());}catch(Throwable ignored){}
        }
    }
}
