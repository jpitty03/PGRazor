package dev.pgrazor.root;

import java.util.HashMap;
import java.util.Map;

/** Real state parser and command builder, with scripts exported for executable shell fixtures. */
public final class RootToggleTest {
    private static void check(boolean value) { if (!value) throw new AssertionError("Root toggle state"); }
    public static void main(String[] args) {
        if (args.length > 0) {
            switch(args[0]) {
                case "on": System.out.print(RootToggleCommand.command(true)); return;
                case "off": System.out.print(RootToggleCommand.command(false)); return;
                case "preserve": System.out.print(RootToggleCommand.preserveOffCommand()); return;
                case "probe": System.out.print(RootModuleState.PROBE); return;
                default: throw new AssertionError("Unknown command");
            }
        }
        Map<String,String> values = new HashMap<>();
        check(!RootModuleState.parse(values,true).canToggle());
        values.put("MODPENDING","1");
        check(RootModuleState.parse(values,true).rebootNeeded);
        check(!RootModuleState.parse(values,true).canToggle());
        values.put("MODVER","v0.2.5-root-fix1");
        check(RootModuleState.parse(values,true).label.contains("update and reboot once"));
        values.put("MODTOGGLE","1");
        check(RootModuleState.parse(values,true).canToggle());
        check(!RootModuleState.parse(values,false).canToggle());
        values.remove("MODPENDING");
        check(!RootModuleState.parse(values,true).rebootNeeded);
        values.put("MODOFF","1");
        check(RootModuleState.parse(values,true).injectionOff);
        check(RootModuleState.parse(values,true).label.contains("off (module stays installed)"));
        values.put("MODDISABLED","1"); check(!RootModuleState.parse(values,true).canToggle());
        values.remove("MODDISABLED"); values.put("MODREMOVE","1");
        check(!RootModuleState.parse(values,true).canToggle());
        check(RootModuleState.parse(values,true).rebootNeeded);
        check(RootToggleCommand.confirmed(false,"PGRAZOR_TOGGLE_OK=off\r\n"));
        check(!RootToggleCommand.confirmed(true,"PGRAZOR_TOGGLE_OK=off\n"));
        check(!RootToggleCommand.confirmed(false,"not PGRAZOR_TOGGLE_OK=off"));
        check(RootToggleCommand.injectionOffFromProbe("MODOFF=1\n"));
        check(!RootToggleCommand.injectionOffFromProbe("MODOFF=10\n"));
        System.out.println("PASS root toggle state");
    }
}
