package dev.pgrazor;

/** Passive, bounded root indicators. Never invokes su or requests elevated access. */
final class RootDiagnostics {
    interface Probe { boolean exists(String path); }
    private static final String[] SU_PATHS={
        "/system/bin/su","/system/xbin/su","/sbin/su","/su/bin/su","/system/bin/.ext/.su"
    };
    static String inspect(int uid,String buildTags,Probe probe) {
        boolean su=false;int failed=0;
        for(String path:SU_PATHS){
            try{su|=probe.exists(path);}catch(SecurityException denied){failed++;}
        }
        boolean testKeys=buildTags!=null&&buildTags.contains("test-keys");
        String status=uid==0?"root-process":su?"indicators-detected":failed>0?"unknown":"not-detected";
        // Test-signed firmware alone is not evidence of working root access.
        return "rootStatus="+status+" rootProcess="+(uid==0)+" suFileVisible="+su
            +" testKeys="+testKeys+" checksFailed="+failed
            +" rootAccess="+(uid==0?"process-uid-zero":"not-tested")
            +" detection=passive visibility=app-sandbox; hidden root may not be detected";
    }
    private RootDiagnostics(){}
}
