import dev.pgrazor.AccountProfileStore;
import dev.pgrazor.AccountProfileStore.Profile;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * Drives the dependency-free {@link AccountProfileStore} against temp directories. No Android types,
 * no device, no network, and no captured account data: identities are synthetic strings and only
 * their SHA-256 ever reaches disk. Each scenario uses its own base directory so a "restart" is just a
 * fresh {@code new AccountProfileStore(base)} over the same files.
 */
public final class AccountProfileStoreTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static String canon(File f) throws IOException {
        return f.getCanonicalPath();
    }

    public static void main(String[] args) throws Exception {
        File root = new File(args[0]);

        legacyDefaultPreserved(new File(root, "legacy"));
        identityLifecycle(new File(root, "lifecycle"));
        perProfileSeparation(new File(root, "separation"));
        corruptionFailsClosed(new File(root, "corrupt"));
        oversizeRejected(new File(root, "oversize"));
        invalidInputsRejected(new File(root, "invalid"));
        globalSettings(new File(root, "global"));

        System.out.println("PASS: AccountProfileStore legacy/identity/separation/restart/corruption/validation");
    }

    private static void globalSettings(File filesDir) throws Exception {
        filesDir.mkdirs();
        AccountProfileStore store = new AccountProfileStore(filesDir);
        Profile alt = store.add("Alt");
        check(!store.globalSettings() && store.mainId().equals(AccountProfileStore.DEFAULT_ID), "global settings default off, main is the default profile");
        check(store.settingsOwner(alt.id).equals(alt.id), "off: each account owns its settings");
        store.setGlobalSettings(true, alt.id);
        check(store.settingsOwner(AccountProfileStore.DEFAULT_ID).equals(alt.id) && store.settingsOwner(alt.id).equals(alt.id), "on: every account uses the main's settings");
        AccountProfileStore reopened = new AccountProfileStore(filesDir);
        check(reopened.globalSettings() && reopened.mainId().equals(alt.id), "global settings survive a restart");
        boolean rejected = false;
        try { reopened.setGlobalSettings(true, "00000000-0000-0000-0000-000000000000"); } catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected && reopened.mainId().equals(alt.id), "an unknown main is refused and nothing changes");
        // Deleting the main hands its role to the current account; global settings stay on.
        reopened.remove(alt.id);
        check(reopened.globalSettings() && reopened.mainId().equals(AccountProfileStore.DEFAULT_ID), "the current account becomes main when the main is deleted");
        reopened.setGlobalSettings(false, AccountProfileStore.DEFAULT_ID);
        check(!new AccountProfileStore(filesDir).globalSettings(), "turning it off persists");
        // An index written before these keys existed loads with global settings off.
        File index = new File(new File(filesDir, "pgrazor-accounts-v1"), "index.properties");
        String text = new String(Files.readAllBytes(index.toPath()), StandardCharsets.ISO_8859_1).replaceAll("(?m)^(globalSettings|main)=.*\\R", "");
        Files.write(index.toPath(), text.getBytes(StandardCharsets.ISO_8859_1));
        AccountProfileStore legacy = new AccountProfileStore(filesDir);
        check(!legacy.globalSettings() && legacy.mainId().equals(AccountProfileStore.DEFAULT_ID), "older index: off, default main");
    }

    private static void legacyDefaultPreserved(File filesDir) throws Exception {
        filesDir.mkdirs();
        // A legacy file already lives directly in filesDir; it must stay reachable in place.
        File legacy = new File(filesDir, "pgrazor-settings.json");
        Files.write(legacy.toPath(), "{}".getBytes(StandardCharsets.UTF_8));

        AccountProfileStore store = new AccountProfileStore(filesDir);
        check(store.list().size() == 1, "A fresh store has exactly the default profile");
        Profile def = store.selected();
        check(AccountProfileStore.DEFAULT_ID.equals(def.id), "Default is selected at startup");
        check(!def.bound(), "Default starts unbound");

        check(canon(store.dataFile(def.id, "pgrazor-settings.json")).equals(canon(legacy)),
                "Default data files resolve in place in filesDir");
        check(store.preferenceName(def.id, "settings").equals("settings"),
                "Default keeps the unsuffixed legacy preference name");
    }

    private static void identityLifecycle(File filesDir) throws Exception {
        filesDir.mkdirs();
        AccountProfileStore s1 = new AccountProfileStore(filesDir);

        // First login binds the unbound default to identity A; selected does not move.
        Profile boundA = s1.observeIdentity("G-AAA-0001", "Ash");
        check(AccountProfileStore.DEFAULT_ID.equals(boundA.id), "First login binds the selected default");
        check(boundA.bound(), "Default is now bound");
        check(s1.selected().id.equals(AccountProfileStore.DEFAULT_ID), "observeIdentity never moves selected");

        // Seeing the same identity again re-uses the existing profile instead of opening a new slot.
        Profile again = s1.observeIdentity("G-AAA-0001", "Ash");
        check(again.id.equals(boundA.id), "Duplicate identity binds the existing profile");
        check(s1.list().size() == 1, "Duplicate identity opens no new slot");
        check(s1.observeIdentity("G-AAA-0001", "Renamed trainer").id.equals(boundA.id),
                "Changing trainer name keeps the same profile");

        // Queue a switch to a freshly added (still unbound) profile B. Selected stays put.
        Profile b = s1.add("Second account");
        check(!b.bound(), "A newly added profile is unbound");
        s1.prepareSwitch(b.id);
        check(s1.selected().id.equals(AccountProfileStore.DEFAULT_ID), "prepareSwitch leaves selected unchanged");
        check(s1.pendingId().equals(b.id), "prepareSwitch records the pending target");

        // Restart: the pending marker must survive a fresh open over the same files.
        AccountProfileStore s2 = new AccountProfileStore(filesDir);
        check(s2.pendingId().equals(b.id), "Pending target survives a restart");
        check(s2.selected().id.equals(AccountProfileStore.DEFAULT_ID), "Selected also survives a restart");
        s2.activatePending();
        check(s2.selected().id.equals(b.id), "activatePending promotes the pending target");
        check(s2.pendingId().isEmpty(), "activatePending clears the pending marker");
        s2.prepareSwitch(boundA.id);
        s2.cancelPending();
        check(new AccountProfileStore(filesDir).pendingId().isEmpty(), "Cancelled switch stays cancelled after restart");
        check(s2.selected().id.equals(b.id), "Cancelling a switch preserves selected profile");

        // Restart into B. The user *intended* B, but actually logs into identity A (the default's).
        // observeIdentity must resolve the ACTUAL profile (default), not the intended one, and must
        // not touch the selected namespace.
        AccountProfileStore s3 = new AccountProfileStore(filesDir);
        check(s3.selected().id.equals(b.id) && !s3.selected().bound(), "Restarted into unbound B");
        Profile actual = s3.observeIdentity("G-AAA-0001", "Ash");
        check(actual.id.equals(boundA.id), "Wrong login resolves the actual profile, not the intended one");
        check(s3.selected().id.equals(b.id), "A mismatched login does not move selected");
        check(!s3.get(b.id).bound(), "A mismatched login does not bind the intended profile");

        // Now a genuine new identity is observed while B (the selected profile) is still unbound:
        // it binds B rather than spawning a slot.
        Profile boundB = s3.observeIdentity("G-BBB-0002", "Gary");
        check(boundB.id.equals(b.id) && boundB.bound(), "A new identity binds the unbound selected profile");
        check(s3.list().size() == 2, "Binding the selected profile opens no new slot");

        // With the selected profile (B) now bound, an unrelated identity must create a NEW profile and
        // must never overwrite B's binding.
        Profile boundC = s3.observeIdentity("G-CCC-0003", "Blue");
        check(!boundC.id.equals(b.id) && !boundC.id.equals(boundA.id), "A third identity creates a new profile");
        check(boundC.bound(), "The new profile is bound");
        check(s3.selected().id.equals(b.id), "Creating a new profile never moves the bound selection");
        check(s3.get(b.id).bound(), "The selected bound profile is never overwritten");
        check(s3.list().size() == 3, "The third identity added exactly one slot");

        // selectVerified requires a bound target and clears pending.
        s3.selectVerified(boundC.id);
        check(s3.selected().id.equals(boundC.id) && s3.pendingId().isEmpty(), "selectVerified selects a bound profile");
        s3.rename(AccountProfileStore.DEFAULT_ID, "Main");
        check(s3.get(AccountProfileStore.DEFAULT_ID).label.equals("Main"), "rename updates the label");
    }

    private static void perProfileSeparation(File filesDir) throws Exception {
        filesDir.mkdirs();
        AccountProfileStore store = new AccountProfileStore(filesDir);
        Profile other = store.add("Alt");

        File defFile = store.dataFile(AccountProfileStore.DEFAULT_ID, "state.json");
        File otherFile = store.dataFile(other.id, "state.json");
        check(!canon(defFile).equals(canon(otherFile)), "Two profiles get separate data files");
        check(canon(defFile).equals(canon(new File(filesDir, "state.json"))), "Default stays in filesDir");
        check(canon(otherFile).startsWith(canon(new File(filesDir, "pgrazor-accounts-v1"))),
                "Non-default files are scoped under the account store");
        check(otherFile.getParentFile().isDirectory(), "The scoped profile directory is created on demand");

        check(store.preferenceName(AccountProfileStore.DEFAULT_ID, "settings").equals("settings"),
                "Default preference name is unsuffixed");
        check(store.preferenceName(other.id, "settings").equals("settings__" + other.id),
                "Non-default preference name carries the profile suffix");
    }

    private static void corruptionFailsClosed(File filesDir) throws Exception {
        File base = new File(filesDir, "pgrazor-accounts-v1");
        base.mkdirs();
        File index = new File(base, "index.properties");
        // A valid version line but missing the required selected/ids fields: must fail, not reset.
        Files.write(index.toPath(), "version=1\nprofile.default.label=X\n".getBytes(StandardCharsets.UTF_8));
        try {
            new AccountProfileStore(filesDir);
            throw new AssertionError("A corrupt index was silently accepted");
        } catch (IOException expected) {
            // Expected: corruption surfaces rather than discarding the mapping.
        }
        check(index.isFile(), "A corrupt index is left in place, never reset");
    }

    private static void oversizeRejected(File filesDir) throws Exception {
        File base = new File(filesDir, "pgrazor-accounts-v1");
        base.mkdirs();
        File index = new File(base, "index.properties");
        byte[] huge = new byte[AccountProfileStore.MAX_INDEX_BYTES + 1];
        java.util.Arrays.fill(huge, (byte) 'a');
        Files.write(index.toPath(), huge);
        try {
            new AccountProfileStore(filesDir);
            throw new AssertionError("An oversize index was accepted");
        } catch (IOException expected) {
            // Expected: the size bound guards against foreign/corrupt files.
        }
    }

    private static void invalidInputsRejected(File filesDir) throws Exception {
        filesDir.mkdirs();
        AccountProfileStore store = new AccountProfileStore(filesDir);

        // Unknown profile id.
        try { store.dataFile("not-a-real-id", "x.json"); throw new AssertionError("Unknown id accepted"); }
        catch (IllegalArgumentException expected) { }

        // Unsafe data file names must be rejected before touching the filesystem.
        for (String bad : new String[]{"../escape", "a/b", "..", ".", "", "name with space", "sub\\win", "tab\tname"}) {
            try { store.dataFile(AccountProfileStore.DEFAULT_ID, bad); throw new AssertionError("Unsafe name accepted: " + bad); }
            catch (IOException expected) { }
        }

        // Unsafe preference scopes are rejected too.
        try { store.preferenceName(AccountProfileStore.DEFAULT_ID, "a/b"); throw new AssertionError("Unsafe scope accepted"); }
        catch (IllegalArgumentException expected) { }

        // Blank labels are rejected.
        try { store.add("   "); throw new AssertionError("Blank label accepted"); }
        catch (IllegalArgumentException expected) { }

        // selectVerified on an unbound profile is refused.
        Profile unbound = store.add("Unbound");
        try { store.selectVerified(unbound.id); throw new AssertionError("Selected an unbound profile"); }
        catch (IllegalStateException expected) { }

        // activatePending with nothing queued is refused.
        try { store.activatePending(); throw new AssertionError("Activated an empty pending"); }
        catch (IllegalStateException expected) { }
    }
}
