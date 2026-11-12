package dev.pgrazor;
public final class RootDiagnosticsTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    public static void main(String[] args){
        String clean=RootDiagnostics.inspect(10001,"release-keys",path->false);
        check(clean.contains("rootStatus=not-detected")&&clean.contains("rootAccess=not-tested"));
        String present=RootDiagnostics.inspect(10001,null,path->path.equals("/system/xbin/su"));
        check(present.contains("rootStatus=indicators-detected")&&present.contains("suFileVisible=true"));
        String denied=RootDiagnostics.inspect(10001,null,path->{throw new SecurityException("private fixture text");});
        check(denied.contains("rootStatus=unknown")&&denied.contains("checksFailed=5")&&!denied.contains("private"));
        String rooted=RootDiagnostics.inspect(0,null,path->false);
        check(rooted.contains("rootStatus=root-process")&&rooted.contains("rootAccess=process-uid-zero"));
        String custom=RootDiagnostics.inspect(10001,"test-keys",path->false);
        check(custom.contains("rootStatus=not-detected")&&custom.contains("testKeys=true"));
        int[] calls={0};RootDiagnostics.inspect(10001,null,path->{calls[0]++;return true;});
        check(calls[0]==5);
        System.out.println("PASS passive root indicators: visible su, absent, denied, UID0, test firmware, bounded probes");
    }
}
