package dev.pgrazor;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.UUID;
import java.util.regex.Pattern;

/**
 * Dependency-free index and app-private storage router for multiple game accounts ("profiles").
 *
 * <p>This class holds <em>no</em> passwords, tokens, or provider account identity. It only records,
 * per profile, a stable id, a user label, a one-way SHA-256 of the in-game identity string (so the
 * same account can be recognised again without storing anything reversible), and a last-used stamp.
 * Globally it records which profile the current run started as ({@link #selected()}) and an optional
 * pending switch target that survives a restart.
 *
 * <p>Profile namespaces never change mid-process. The host captures the startup profile once; an
 * account switch is performed by restarting the process so stale callbacks can never write into a
 * different profile's files. The flow is: {@link #prepareSwitch(String)} durably marks intent but
 * leaves {@link #selected()} alone; after the host has verified a clean logout it calls
 * {@link #activatePending()} (or, having observed the game identity, {@link #selectVerified(String)})
 * and restarts.
 *
 * <p>The legacy single-account layout is preserved: profile {@code "default"} reads and writes its
 * files directly in {@code filesDir} (and uses unsuffixed SharedPreferences names), so existing
 * installs keep their data in place. Every other profile has a random-UUID id and is scoped under
 * {@code pgrazor-accounts-v1/profiles/<id>/}.
 *
 * <p>All writes are a single atomic, durable transaction: the new index is serialised, flushed to
 * stable storage, and only then moved into place; the in-memory snapshot is replaced only after the
 * on-disk move succeeds, so a failure leaves both the previous file and the previous snapshot intact.
 *
 * <p>No Android types are referenced; the host passes a {@link File} base directory
 * (e.g. {@code context.getFilesDir()}). This is Java 8 / Android API 28 compatible. There is
 * Deletion is limited to inactive profiles. Call these methods off the UI thread.
 */
public final class AccountProfileStore {
    /** Id of the legacy, in-place profile that owns the existing single-account files. */
    public static final String DEFAULT_ID = "default";
    /** Largest number of profiles the index will hold. */
    public static final int MAX_PROFILES = 64;
    /** Largest index file the loader will accept, as a corruption/foreign-file guard. */
    public static final int MAX_INDEX_BYTES = 128 * 1024;

    private static final String DIR = "pgrazor-accounts-v1";
    private static final String PROFILES_DIR = "profiles";
    private static final String INDEX_NAME = "index.properties";
    private static final String INDEX_VERSION = "1";

    private static final String KEY_VERSION = "version";
    private static final String KEY_SELECTED = "selected";
    private static final String KEY_PENDING = "pending";
    private static final String KEY_IDS = "ids";
    // Optional (older indexes have neither): whether every account uses the main account's settings.
    private static final String KEY_GLOBAL = "globalSettings";
    private static final String KEY_MAIN = "main";
    private static final String PREFIX = "profile.";

    private static final Pattern UUID_RE = Pattern.compile(
            "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    private static final Pattern SAFE_NAME_RE = Pattern.compile("[A-Za-z0-9._-]+");
    private static final Pattern HEX64_RE = Pattern.compile("[0-9a-f]{64}");

    private final File filesDir;
    private final File baseDir;
    private final File indexFile;

    // In-memory snapshot. Replaced wholesale only after a successful durable write.
    private LinkedHashMap<String, Profile> profiles = new LinkedHashMap<>();
    private String selected = DEFAULT_ID;
    private String pending = "";
    private boolean globalSettings = false;
    private String mainId = DEFAULT_ID;

    /**
     * Opens (or, on first use, bootstraps) the index under {@code filesDir}.
     *
     * @throws IOException if an existing index is unreadable or fails validation. A corrupt index is
     *     never silently reset, so the host can surface the problem rather than lose the mapping.
     */
    public AccountProfileStore(File filesDir) throws IOException {
        if (filesDir == null) throw new IllegalArgumentException("filesDir");
        this.filesDir = filesDir;
        this.baseDir = new File(filesDir, DIR);
        this.indexFile = new File(baseDir, INDEX_NAME);
        if (indexFile.isFile()) {
            load();
        } else {
            // First run (or a fresh install upgrading from the legacy single-account layout):
            // seed the in-place default profile and persist it so a later restart is stable.
            profiles.put(DEFAULT_ID, new Profile(DEFAULT_ID, "Default", null, System.currentTimeMillis()));
            selected = DEFAULT_ID;
            pending = "";
            persist(profiles, selected, pending);
        }
    }

    /** An immutable view of one profile. All fields are copies; the store is never mutated through it. */
    public static final class Profile {
        /** Stable id: {@link #DEFAULT_ID} for the legacy profile, otherwise a random UUID. */
        public final String id;
        /** User-visible label. */
        public final String label;
        /** SHA-256 (lower-case hex) of the in-game identity this profile is bound to, or {@code null}. */
        public final String identityHash;
        /** Epoch-millis stamp of the last time this profile was selected/observed. */
        public final long lastUsed;

        Profile(String id, String label, String identityHash, long lastUsed) {
            this.id = id;
            this.label = label;
            this.identityHash = identityHash;
            this.lastUsed = lastUsed;
        }

        /** True once this profile has been bound to a concrete in-game identity. */
        public boolean bound() {
            return identityHash != null && !identityHash.isEmpty();
        }

        Profile copy() {
            return new Profile(id, label, identityHash, lastUsed);
        }
    }

    /** All profiles in stored order (default first). Returned objects are copies. */
    public synchronized List<Profile> list() {
        List<Profile> out = new ArrayList<>(profiles.size());
        for (Profile p : profiles.values()) out.add(p.copy());
        return Collections.unmodifiableList(out);
    }

    /** The profile this run started as; always present. Returned as a copy. */
    public synchronized Profile selected() {
        return profiles.get(selected).copy();
    }

    /** Whether every account uses the main account's settings (off by default). */
    public synchronized boolean globalSettings() {
        return globalSettings;
    }

    /** The main account: the one whose settings every account uses while global settings are on. */
    public synchronized String mainId() {
        return mainId;
    }

    /**
     * The profile whose settings file {@code profileId} reads and writes: the main account while
     * global settings are on, otherwise the profile itself. Each account's own file is left as it
     * was, so turning global settings off brings its own settings back.
     */
    public synchronized String settingsOwner(String profileId) {
        requireKnownId(profileId);
        return globalSettings ? mainId : profileId;
    }

    /** Turns global settings on or off and chooses the main account, durably and together. */
    public synchronized void setGlobalSettings(boolean on, String main) throws IOException {
        requireKnownId(main);
        persist(profiles, selected, pending, on, main);
        globalSettings = on;
        mainId = main;
    }

    /** The id of the pending switch target, or {@code ""} when none is queued. */
    public synchronized String pendingId() {
        return pending;
    }

    /** The profile with this id, or {@code null} if unknown. Returned as a copy. */
    public synchronized Profile get(String id) {
        Profile p = (id == null) ? null : profiles.get(id);
        return p == null ? null : p.copy();
    }

    /** Creates a new, unbound profile with a random UUID and the given label. */
    public synchronized Profile add(String label) throws IOException {
        String clean = requireLabel(label);
        if (profiles.size() >= MAX_PROFILES)
            throw new IOException("The profile index is full (" + MAX_PROFILES + ").");
        String id;
        do { id = UUID.randomUUID().toString(); } while (profiles.containsKey(id));
        LinkedHashMap<String, Profile> next = copyProfiles();
        Profile created = new Profile(id, clean, null, System.currentTimeMillis());
        next.put(id, created);
        persist(next, selected, pending);
        commit(next, selected, pending);
        return created.copy();
    }

    /** Changes a profile's label. The id and binding are unchanged. */
    public synchronized void rename(String id, String label) throws IOException {
        String clean = requireLabel(label);
        Profile current = profiles.get(id);
        if (current == null) throw new IllegalArgumentException("Unknown profile id.");
        LinkedHashMap<String, Profile> next = copyProfiles();
        next.put(id, new Profile(current.id, clean, current.identityHash, current.lastUsed));
        persist(next, selected, pending);
        commit(next, selected, pending);
    }

    /** Protects active state and any in-flight switch before storage is removed. */
    public synchronized void requireRemovable(String id) {
        requireKnownId(id);
        if(selected.equals(id))throw new IllegalStateException("Switch accounts before deleting the current account.");
        if(!pending.isEmpty())throw new IllegalStateException("An account switch is pending.");
    }

    /** Removes an inactive index entry after its saved session and local data are deleted. */
    public synchronized void remove(String id) throws IOException {
        requireRemovable(id);
        LinkedHashMap<String,Profile> next=copyProfiles();next.remove(id);
        // The main account cannot vanish under global settings: the current account takes over.
        final String nextMain=mainId.equals(id)?selected:mainId;
        persist(next,selected,pending,globalSettings,nextMain);commit(next,selected,pending);mainId=nextMain;
    }

    /**
     * Durably records intent to switch to {@code targetId} on the next verified logout/restart.
     * Does <em>not</em> change {@link #selected()}; the active namespace stays put for this run.
     */
    public synchronized void prepareSwitch(String targetId) throws IOException {
        if (!profiles.containsKey(targetId)) throw new IllegalArgumentException("Unknown profile id.");
        persist(profiles, selected, targetId);
        commit(profiles, selected, targetId);
    }

    /**
     * Promotes the pending target to {@link #selected()} and clears the pending marker. The host
     * calls this after it has verified a clean logout, immediately before restarting.
     */
    public synchronized void activatePending() throws IOException {
        if (pending.isEmpty()) throw new IllegalStateException("No pending switch to activate.");
        if (!profiles.containsKey(pending)) throw new IllegalStateException("Pending target no longer exists.");
        persist(profiles, pending, "");
        commit(profiles, pending, "");
    }

    /** Cancels an interrupted request without selecting or deleting any profile. */
    public synchronized void cancelPending() throws IOException {
        if(pending.isEmpty())return;
        persist(profiles,selected,"");
        commit(profiles,selected,"");
    }

    /**
     * Records which profile the just-observed in-game identity actually belongs to, selecting it and
     * clearing any pending marker. The target must already be bound to an identity. The host calls
     * this after {@link #observeIdentity} and then restarts.
     */
    public synchronized void selectVerified(String id) throws IOException {
        Profile target = profiles.get(id);
        if (target == null) throw new IllegalArgumentException("Unknown profile id.");
        if (!target.bound()) throw new IllegalStateException("Profile is not bound to an identity.");
        persist(profiles, id, "");
        commit(profiles, id, "");
    }

    /**
     * Resolves the in-game identity seen at startup to a profile, without ever changing
     * {@link #selected()} to a different identity. Resolution order:
     * <ol>
     *   <li>If an existing profile is already bound to this identity, return it (so a duplicate login
     *       re-uses that profile rather than opening a new slot).</li>
     *   <li>Otherwise, if the selected profile is still unbound, bind it to this identity and return
     *       it (first login on a legacy/default or freshly-added profile).</li>
     *   <li>Otherwise, create a new profile bound to this identity and return it — the selected, bound
     *       profile is never overwritten by a mismatched login.</li>
     * </ol>
     * Only a SHA-256 of the identity is persisted; {@code rawGameId}/{@code trainerName} are not stored.
     */
    public synchronized Profile observeIdentity(String rawGameId, String trainerName) throws IOException {
        String hash = identityHash(rawGameId, trainerName);
        long now = System.currentTimeMillis();

        for (Profile p : profiles.values()) {
            if (hash.equals(p.identityHash)) {
                LinkedHashMap<String, Profile> next = copyProfiles();
                Profile touched = new Profile(p.id, p.label, p.identityHash, now);
                next.put(p.id, touched);
                persist(next, selected, pending);
                commit(next, selected, pending);
                return touched.copy();
            }
        }

        Profile current = profiles.get(selected);
        if (current != null && !current.bound()) {
            LinkedHashMap<String, Profile> next = copyProfiles();
            Profile bound = new Profile(current.id, current.label, hash, now);
            next.put(current.id, bound);
            persist(next, selected, pending);
            commit(next, selected, pending);
            return bound.copy();
        }

        if (profiles.size() >= MAX_PROFILES)
            throw new IOException("The profile index is full (" + MAX_PROFILES + ").");
        String id;
        do { id = UUID.randomUUID().toString(); } while (profiles.containsKey(id));
        LinkedHashMap<String, Profile> next = copyProfiles();
        Profile created = new Profile(id, defaultLabel(profiles.size()), hash, now);
        next.put(id, created);
        persist(next, selected, pending);
        commit(next, selected, pending);
        return created.copy();
    }

    /**
     * Resolves an app-private data file for a profile. {@code name} must be a safe base filename
     * (ASCII letters/digits and {@code . _ -} only; no slash, no {@code ..}, no traversal). The
     * default profile maps to {@code filesDir/name} in place; every other profile is scoped under its
     * own directory, which is created on demand.
     */
    public synchronized File dataFile(String profileId, String name) throws IOException {
        requireKnownId(profileId);
        if (!isSafeName(name)) throw new IOException("Unsafe data file name.");
        File target;
        if (DEFAULT_ID.equals(profileId)) {
            target = new File(filesDir, name);
        } else {
            File dir = new File(new File(baseDir, PROFILES_DIR), profileId);
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Could not create profile storage.");
            target = new File(dir, name);
        }
        // Defence in depth: the resolved path must stay inside its intended root.
        File root = DEFAULT_ID.equals(profileId) ? filesDir : new File(new File(baseDir, PROFILES_DIR), profileId);
        if (!target.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator))
            throw new IOException("Resolved path escapes its profile root.");
        return target;
    }

    /**
     * Returns the SharedPreferences name a profile should use for the given logical {@code scope}.
     * The default profile keeps the unsuffixed legacy name; others get a per-id suffix so their
     * preferences stay separate. {@code scope} must be a safe base name.
     */
    public synchronized String preferenceName(String profileId, String scope) {
        requireKnownId(profileId);
        if (!isSafeName(scope)) throw new IllegalArgumentException("Unsafe preference scope.");
        if (DEFAULT_ID.equals(profileId)) return scope;
        return scope + "__" + profileId;
    }

    // ---- internals -------------------------------------------------------------------------------

    private void requireKnownId(String id) {
        if (id == null || !profiles.containsKey(id)) throw new IllegalArgumentException("Unknown profile id.");
    }

    private static String requireLabel(String label) {
        if (label == null) throw new IllegalArgumentException("Label is required.");
        String trimmed = label.trim();
        if (trimmed.isEmpty()) throw new IllegalArgumentException("Label is required.");
        if (trimmed.length() > 200) throw new IllegalArgumentException("Label is too long.");
        return trimmed;
    }

    private static String defaultLabel(int existing) {
        return "Account " + (existing + 1);
    }

    private static boolean isSafeName(String name) {
        return name != null
                && !name.isEmpty()
                && !name.equals(".")
                && !name.equals("..")
                && name.indexOf('/') < 0
                && name.indexOf('\\') < 0
                && SAFE_NAME_RE.matcher(name).matches();
    }

    private static boolean isValidId(String id) {
        return DEFAULT_ID.equals(id) || (id != null && UUID_RE.matcher(id).matches());
    }

    private static String identityHash(String rawGameId, String trainerName) {
        String id = rawGameId == null ? "" : rawGameId.trim();
        if (id.isEmpty()) throw new IllegalArgumentException("Empty game identity.");
        String identity = id;
        try {
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            byte[] digest = sha.digest(identity.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder(64);
            for (byte b : digest) {
                int v = b & 0xff;
                hex.append(Character.forDigit(v >>> 4, 16));
                hex.append(Character.forDigit(v & 0xf, 16));
            }
            return hex.toString();
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException("SHA-256 unavailable", impossible);
        }
    }

    private LinkedHashMap<String, Profile> copyProfiles() {
        LinkedHashMap<String, Profile> copy = new LinkedHashMap<>();
        for (Map.Entry<String, Profile> e : profiles.entrySet()) copy.put(e.getKey(), e.getValue().copy());
        return copy;
    }

    /** Swaps in the new snapshot. Only ever called after {@link #persist} has returned. */
    private void commit(LinkedHashMap<String, Profile> next, String nextSelected, String nextPending) {
        this.profiles = next;
        this.selected = nextSelected;
        this.pending = nextPending;
    }

    /**
     * Serialises the candidate state and writes it atomically and durably. Returns only on success;
     * on any failure the previous index file is left untouched and the caller must not commit.
     */
    private void persist(LinkedHashMap<String, Profile> next, String nextSelected, String nextPending)
            throws IOException {
        persist(next, nextSelected, nextPending, globalSettings, mainId);
    }

    private void persist(LinkedHashMap<String, Profile> next, String nextSelected, String nextPending,
                         boolean nextGlobal, String nextMain) throws IOException {
        Properties props = new Properties();
        props.setProperty(KEY_VERSION, INDEX_VERSION);
        props.setProperty(KEY_SELECTED, nextSelected);
        props.setProperty(KEY_PENDING, nextPending);
        props.setProperty(KEY_GLOBAL, Boolean.toString(nextGlobal));
        props.setProperty(KEY_MAIN, nextMain);
        StringBuilder ids = new StringBuilder();
        for (Profile p : next.values()) {
            if (ids.length() > 0) ids.append(',');
            ids.append(p.id);
            props.setProperty(PREFIX + p.id + ".label", p.label);
            props.setProperty(PREFIX + p.id + ".hash", p.identityHash == null ? "" : p.identityHash);
            props.setProperty(PREFIX + p.id + ".lastUsed", Long.toString(p.lastUsed));
        }
        props.setProperty(KEY_IDS, ids.toString());

        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        props.store(buffer, "pgrazor account index v" + INDEX_VERSION + " - no secrets stored");
        byte[] bytes = buffer.toByteArray();
        if (bytes.length > MAX_INDEX_BYTES) throw new IOException("Index would exceed the size bound.");

        if (!baseDir.isDirectory() && !baseDir.mkdirs()) throw new IOException("Could not create the account store.");
        Path dir = baseDir.toPath();
        Path temp = Files.createTempFile(dir, "index", ".tmp");
        boolean moved = false;
        try {
            try (FileChannel channel = FileChannel.open(temp,
                    StandardOpenOption.WRITE, StandardOpenOption.TRUNCATE_EXISTING)) {
                java.nio.ByteBuffer data=java.nio.ByteBuffer.wrap(bytes);
                while(data.hasRemaining())channel.write(data);
                channel.force(true);
            }
            try {
                Files.move(temp, indexFile.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException fallback) {
                throw new IOException("Atomic account save unavailable", fallback);
            }
            moved = true;
        } finally {
            if (!moved) Files.deleteIfExists(temp);
        }
    }

    /** Reads and strictly validates the on-disk index into the in-memory snapshot. */
    private void load() throws IOException {
        if(Files.size(indexFile.toPath())>MAX_INDEX_BYTES)throw new IOException("Index exceeds size bound");
        byte[] bytes = Files.readAllBytes(indexFile.toPath());
        if (bytes.length > MAX_INDEX_BYTES) throw new IOException("Index exceeds the size bound; refusing to load.");

        Properties props = new Properties();
        props.load(new ByteArrayInputStream(bytes));

        String version = props.getProperty(KEY_VERSION);
        if (!INDEX_VERSION.equals(version)) throw new IOException("Unsupported index version: " + version);

        String sel = props.getProperty(KEY_SELECTED);
        String pend = props.getProperty(KEY_PENDING);
        String idsRaw = props.getProperty(KEY_IDS);
        if (sel == null || pend == null || idsRaw == null) throw new IOException("Index is missing required fields.");

        LinkedHashMap<String, Profile> loaded = new LinkedHashMap<>();
        if (!idsRaw.isEmpty()) {
            String[] ids = idsRaw.split(",", -1);
            if (ids.length > MAX_PROFILES) throw new IOException("Index holds too many profiles.");
            java.util.HashSet<String> seenHashes = new java.util.HashSet<>();
            for (String id : ids) {
                if (!isValidId(id)) throw new IOException("Index holds an invalid profile id.");
                if (loaded.containsKey(id)) throw new IOException("Index holds a duplicate profile id.");
                String label = props.getProperty(PREFIX + id + ".label");
                String hash = props.getProperty(PREFIX + id + ".hash");
                String lastUsedRaw = props.getProperty(PREFIX + id + ".lastUsed");
                if (label == null || hash == null || lastUsedRaw == null)
                    throw new IOException("Index is missing fields for profile " + id + ".");
                if (label.trim().isEmpty()) throw new IOException("Index holds a blank label.");
                String identityHash;
                if (hash.isEmpty()) {
                    identityHash = null;
                } else {
                    if (!HEX64_RE.matcher(hash).matches()) throw new IOException("Index holds a malformed identity hash.");
                    if (!seenHashes.add(hash)) throw new IOException("Index binds two profiles to one identity.");
                    identityHash = hash;
                }
                long lastUsed;
                try {
                    lastUsed = Long.parseLong(lastUsedRaw);
                } catch (NumberFormatException bad) {
                    throw new IOException("Index holds a malformed lastUsed stamp.");
                }
                if (lastUsed < 0) throw new IOException("Index holds a negative lastUsed stamp.");
                loaded.put(id, new Profile(id, label, identityHash, lastUsed));
            }
        }

        if (loaded.isEmpty()) throw new IOException("Index has no profiles.");
        if (!loaded.containsKey(sel)) throw new IOException("Index selects an unknown profile.");
        if (!pend.isEmpty() && !loaded.containsKey(pend)) throw new IOException("Index points at an unknown pending profile.");

        this.profiles = loaded;
        this.selected = sel;
        this.pending = pend;
        // Optional fields: absent (an older index) means off; an unknown main falls back safely.
        String main = props.getProperty(KEY_MAIN);
        this.mainId = main != null && loaded.containsKey(main) ? main : loaded.containsKey(DEFAULT_ID) ? DEFAULT_ID : sel;
        this.globalSettings = "true".equals(props.getProperty(KEY_GLOBAL));
    }
}
