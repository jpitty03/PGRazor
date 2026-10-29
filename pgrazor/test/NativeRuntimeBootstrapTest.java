package dev.pgrazor;

/** Actual helper state-machine tests; native discovery is deliberately stubbed. */
public final class NativeRuntimeBootstrapTest {
    private static long result(int status, int detail) {
        return ((long) status << 32) | (detail & 0xffffffffL);
    }
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void terminal() {
        int calls=NativeCore.calls;
        NativeRuntimeBootstrap.start();
        NativeRuntimeBootstrap.onRendered();
        NativeRuntimeBootstrap.start();
        NativeRuntimeBootstrap.onRendered();
        check(NativeCore.calls==calls);
    }
    public static void main(String[] args) throws Exception {
        String scenario=args[0];
        if (scenario.equals("unarmed")) {
            NativeRuntimeBootstrap.onRendered(); NativeRuntimeBootstrap.onRendered();
            check(NativeCore.calls==0 && Control.events.isEmpty());
        } else {
            NativeRuntimeBootstrap.start(); NativeRuntimeBootstrap.start();
            switch(scenario) {
            case "gameplay":
                NativeRuntimeBootstrap.startGameplay();
                NativeCore.value=result(1,0);NativeRuntimeBootstrap.onRendered();
                check(NativeCore.gameplayCalls==0);
                NativeCore.value=0;NativeCore.gameplayStatus=1;NativeRuntimeBootstrap.onRendered();
                check(NativeCore.gameplayCalls==1);
                NativeCore.gameplayCallback=NativeRuntimeBootstrap::onRendered;
                NativeCore.gameplayStatus=0;NativeRuntimeBootstrap.onRendered();
                check(NativeCore.gameplayCalls==2);
                NativeRuntimeBootstrap.startGameplay();NativeRuntimeBootstrap.onRendered();
                check(NativeCore.gameplayCalls==2);
                check(Control.events.get(Control.events.size()-1).startsWith("native-scheduler-ready|"));break;
            case "gameplay-error":
                NativeRuntimeBootstrap.startGameplay();NativeCore.gameplayStatus=-7;
                NativeRuntimeBootstrap.onRendered();NativeRuntimeBootstrap.onRendered();
                check(NativeCore.gameplayCalls==1);
                check(Control.events.get(Control.events.size()-1).equals("native-scheduler-unavailable|status=-7"));break;
            case "ready":
                NativeRuntimeBootstrap.onRendered();
                check(NativeCore.calls==1 && Control.events.size()==1);
                check(Control.events.get(0).startsWith("native-bootstrap-ready|"));
                terminal(); break;
            case "retry":
                NativeCore.value=result(1,12); NativeRuntimeBootstrap.onRendered();
                NativeCore.value=result(-5,34); NativeRuntimeBootstrap.onRendered();
                check(NativeCore.calls==2 && Control.events.isEmpty());
                NativeCore.value=result(0,0); NativeRuntimeBootstrap.onRendered();
                check(NativeCore.calls==3 && Control.events.size()==1); terminal(); break;
            case "error":
                NativeCore.value=result(-10,-2147483647); NativeRuntimeBootstrap.onRendered();
                check(Control.events.size()==1);
                check(Control.events.get(0).equals("native-bootstrap-unavailable|status=-10 detail=-2147483647"));
                terminal(); break;
            case "throwable":
                NativeCore.failure=new LinkageError("private fixture text");
                NativeRuntimeBootstrap.onRendered();
                check(Control.events.size()==1 && Control.events.get(0).equals("native-bootstrap-unavailable|LinkageError"));
                terminal(); break;
            case "event-throwable":
                Control.failure=new AssertionError("private event text");
                NativeRuntimeBootstrap.onRendered();
                check(NativeCore.calls==1 && Control.events.size()==2);
                check(Control.events.get(1).equals("native-bootstrap-unavailable|AssertionError"));
                terminal(); break;
            case "reentry":
                NativeCore.callback=()->{NativeRuntimeBootstrap.start(); NativeRuntimeBootstrap.onRendered();};
                Control.callback=()->{NativeRuntimeBootstrap.start(); NativeRuntimeBootstrap.onRendered();};
                NativeRuntimeBootstrap.onRendered();
                check(NativeCore.calls==1 && Control.events.size()==1); terminal(); break;
            case "concurrent":
                java.util.concurrent.CountDownLatch entered=new java.util.concurrent.CountDownLatch(1);
                java.util.concurrent.CountDownLatch release=new java.util.concurrent.CountDownLatch(1);
                NativeCore.callback=()->{entered.countDown(); try {check(release.await(5,java.util.concurrent.TimeUnit.SECONDS));}
                    catch(InterruptedException e){throw new AssertionError(e);}};
                Thread worker=new Thread(NativeRuntimeBootstrap::onRendered); worker.start();
                check(entered.await(5,java.util.concurrent.TimeUnit.SECONDS));
                NativeRuntimeBootstrap.onRendered(); NativeRuntimeBootstrap.start();
                release.countDown(); worker.join(5000); check(!worker.isAlive());
                check(NativeCore.calls==1 && Control.events.size()==1); terminal(); break;
            default: throw new AssertionError(scenario);
            }
        }
        System.out.println("PASS NativeRuntimeBootstrap "+scenario);
    }
}
