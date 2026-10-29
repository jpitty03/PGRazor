import com.nianticlabs.background.configuration.FaultyDialog;
import com.nianticlabs.background.configuration.FinalDialog;
import com.nianticlabs.background.configuration.InitialDialog;
import com.nianticlabs.background.configuration.ManualPermissionDialog;
import com.nianticlabs.platform.niaadvsynccore.permission.Permissions;
import com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus;
import dev.pgrazor.Control;
import dev.pgrazor.GameLocationPermissions;

/**
 * Exercises the production {@link GameLocationPermissions} reflective policy against narrow
 * behavioral shells. This drives the real compiled class, not a source mirror.
 */
public class GameLocationPermissionsTest {
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        Control.desired = true;
        GameLocationPermissions helper = GameLocationPermissions.create();

        // Only the exact fine/coarse foreground names are answered, and only while enabled.
        check(helper.locationPermission("android.permission.ACCESS_FINE_LOCATION"), "fine granted");
        check(helper.locationPermission("android.permission.ACCESS_COARSE_LOCATION"), "coarse granted");
        check(!helper.locationPermission("android.permission.ACCESS_BACKGROUND_LOCATION"), "background rejected");
        check(!helper.locationPermission("android.permission.CAMERA"), "camera rejected");
        check(!helper.locationPermission(null), "null permission rejected");

        // The location/precise flags map to the status values; anything else is no override.
        check(helper.permissionStatus(1L) == PermissionStatus.GRANTED_IN_USE, "location flag -> in use enum");
        check(helper.permissionStatus(9007199254740993L) == PermissionStatus.GRANTED_ALWAYS, "precise flag preserves long");
        check(helper.permissionStatus(9007199254740992L) == null, "rounded adjacent flag rejected");
        check(helper.permissionStatus(99L) == null, "unrelated flag -> null");
        check(helper.permissionStatus(0L) == null, "zero flag -> null");

        // Disabled preference suppresses every override, leaving OS state alone.
        Control.desired = false;
        check(!helper.locationPermission("android.permission.ACCESS_FINE_LOCATION"), "disabled -> no fine");
        check(helper.permissionStatus(1L) == null, "disabled -> no status");
        Control.desired = true;

        // Shutdown is immediate and permanent even while the preference is enabled.
        GameLocationPermissions dead = GameLocationPermissions.create();
        dead.shutdown();
        check(!dead.locationPermission("android.permission.ACCESS_FINE_LOCATION"), "shutdown -> no fine");
        check(dead.permissionStatus(1L) == null, "shutdown -> no status");

        // --- Adventure Sync dismissal ---------------------------------------------------------
        Control.events.clear();
        InitialDialog initial = new InitialDialog();
        helper.dismissAdventureSync(initial, "onClickCancel");
        check(initial.cancelled, "denied Initial dialog cancelled");
        ManualPermissionDialog manual = new ManualPermissionDialog();
        helper.dismissAdventureSync(manual, "onClickCancel");
        check(manual.cancelled, "denied Manual dialog cancelled");
        FinalDialog last = new FinalDialog();
        helper.dismissAdventureSync(last, "onClickClose");
        check(last.closed, "denied Final dialog closed");
        check(Control.events.size() == 3, "one dismissal event each");

        // A real grant on either axis leaves the dialog untouched: OS checks are read, never changed.
        InitialDialog fineGranted = new InitialDialog();
        fineGranted.fine = 0;
        helper.dismissAdventureSync(fineGranted, "onClickCancel");
        check(!fineGranted.cancelled, "fine granted -> not dismissed");
        InitialDialog coarseGranted = new InitialDialog();
        coarseGranted.coarse = 0;
        helper.dismissAdventureSync(coarseGranted, "onClickCancel");
        check(!coarseGranted.cancelled, "coarse granted -> not dismissed");

        // Finishing activities, mismatched actions and non-whitelisted callers are all left alone.
        InitialDialog finishing = new InitialDialog();
        finishing.finishing = true;
        helper.dismissAdventureSync(finishing, "onClickCancel");
        check(!finishing.cancelled, "finishing -> not dismissed");
        InitialDialog wrongAction = new InitialDialog();
        helper.dismissAdventureSync(wrongAction, "onClickClose");
        check(!wrongAction.cancelled, "wrong action -> not dismissed");
        FinalDialog wrongFinal = new FinalDialog();
        helper.dismissAdventureSync(wrongFinal, "onClickCancel");
        check(!wrongFinal.closed, "wrong action on final -> not dismissed");
        helper.dismissAdventureSync(new android.app.Activity(), "onClickCancel"); // unknown caller, no action
        helper.dismissAdventureSync(null, "onClickCancel"); // null caller, no throw

        // Disabled and shut-down helpers never dismiss.
        Control.desired = false;
        InitialDialog disabled = new InitialDialog();
        helper.dismissAdventureSync(disabled, "onClickCancel");
        check(!disabled.cancelled, "disabled -> not dismissed");
        Control.desired = true;
        InitialDialog afterStop = new InitialDialog();
        dead.dismissAdventureSync(afterStop, "onClickCancel");
        check(!afterStop.cancelled, "shutdown -> not dismissed");

        // A reflective failure during the checks is contained and logged, never thrown.
        Control.events.clear();
        FaultyDialog faulty = new FaultyDialog();
        helper.dismissAdventureSync(faulty, "onClickCancel");
        check(!faulty.cancelled, "faulty dialog not cancelled");
        check(Control.events.size() == 1 && Control.events.get(0).startsWith("adventure-sync-dismiss-error"),
            "reflection failure contained and logged");

        System.out.println("PASS game location permissions: exact fine/coarse gating, flag->status mapping with "
            + "disabled/shutdown fallback, enum init forced, Adventure Sync dismissal only on denied OS grant for "
            + "whitelisted dialogs/actions, OS checks untouched, reflection errors contained");
    }
}
