package dev.pgrazor;

/** Read-only scheduler progress classification. No hook repair or gameplay decisions. */
final class RuntimeHealth {
    private long lastPulses=-1, lastGeneration=-1, progressAt, reportedAt;
    private boolean wasForeground, observedProgress;
    private String lastState="";

    // Native snapshot: installed, stopping, fault, pulses, generation, ownerTid, readyMask.
    String sample(long nowMs, boolean foreground, long[] nativeState) {
        if(nativeState==null||nativeState.length!=7)return null;
        long pulses=nativeState[3], generation=nativeState[4];
        boolean reset=lastPulses<0||generation!=lastGeneration||(!wasForeground&&foreground);
        if(reset){progressAt=nowMs;observedProgress=false;}
        else if(pulses!=lastPulses){progressAt=nowMs;observedProgress=true;}
        lastPulses=pulses;lastGeneration=generation;wasForeground=foreground;
        long age=Math.max(0,nowMs-progressAt);
        String state=nativeState[2]!=0?"faulted":nativeState[1]!=0?"stopped":nativeState[0]==0?"not-installed"
            :!foreground?"background":age>=15000?"no-pulse-progress":observedProgress?"progressing":"observing";
        if(state.equals(lastState)&&nowMs-reportedAt<60000)return null;
        lastState=state;reportedAt=nowMs;
        return "state="+state+" installed="+nativeState[0]+" stopping="+nativeState[1]+" fault="+nativeState[2]
            +" pulses="+pulses+" pulseProgressAgeMs="+age+" generation="+generation
            +" ownerTid="+nativeState[5]+" readyMask="+nativeState[6]+" foregroundGate="+foreground
            +"; no-pulse-progress alone does not prove a detached hook";
    }
}
