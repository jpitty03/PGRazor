package dev.pgrazor;

/** Real Control policy: Silver Pinap is 708; Golden Nanab 707 is not its substitute. */
public final class ControlSilverPinapTest {
    private static void check(boolean value, String why) { if (!value) throw new AssertionError(why); }
    public static void main(String[] args) throws Exception {
        android.app.Application app=new android.app.Application() {
            public java.io.File getFilesDir(){return new java.io.File(args[0]);}
        };
        java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);
        load.setAccessible(true);load.invoke(null,app);
        Control.setBuddyBerry(0);
        Control.setBuddyBerry(707);
        check(Control.buddyBerry() == 0, "707 cannot enable the Silver Pinap buddy option");
        Control.setBuddyBerry(708);
        check(Control.buddyBerry() == 708, "buddy choice accepts 708");
        Control.setBuddyBerry(707);
        check(Control.buddyBerry() == 708, "invalid choice cannot overwrite a valid selection");
        Control.setAutoBerry(0);
        Control.selectBerryPriority(707);
        check(Control.berryPriority().isEmpty(), "707 cannot enter the catch picker");
        Control.selectBerryPriority(708);
        Control.selectBerryPriority(705);
        check(Control.berryPriority().equals("708,705"), "Silver Pinap priority retains exact ID and order");
        Control.setItemCatalog("707|Golden Nanab Berry|99\n708|Silver Pinap Berry|0\n705|Pinap Berry|2");
        check(Control.autoBerrySelected() == 705, "stock at 707 cannot satisfy the 708 choice");
        Control.setItemCatalog("707|Golden Nanab Berry|99\n708|Silver Pinap Berry|3\n705|Pinap Berry|2");
        check(Control.autoBerrySelected() == 708 && Control.berryCount(708) == 3, "708 stock selects Silver Pinap first");
        Control.setEncounterBerryEnabled(false);
        check(Control.autoBerry() == 0 && Control.effectiveBerryPriority().isEmpty(), "OFF still blocks feeding");
        Control.setEncounterBerryEnabled(true);
        check(Control.autoBerry() == 708 && Control.effectiveBerryPriority().equals("708,705"), "native reader receives 708");
        Control.flushSettings(5000);
        check(Control.exportSettings().getProperty("buddyBerry").equals("708"), "saved buddy selection uses 708");
        check(Control.exportSettings().getProperty("berryPriority").equals("708,705"), "saved catch priority uses 708");
        System.out.println("PASS Silver Pinap: exact buddy/catch IDs, stock isolation, fallback, OFF gate and saved preferences");
    }
}
