package dev.pgrazor;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import java.io.InputStream;

/**
 * Decodes and caches the small bundled icons drawn in {@link LiveFeedDialog}'s feed rows.
 *
 * <p>Pure {@code AssetManager} reads — never the network — with decoded assets reused while cached and
 * held in a bounded {@link LruCache}. A shared {@link #MISSING} sentinel is cached for absent files
 * so a missing asset is not re-opened on every bind; callers fall back to a drawn glyph when this
 * returns {@code null}. Instances are cheap and hold only the application context; access is expected
 * from the single UI thread that binds list rows.
 */
public final class LiveFeedIcons {
    /** Base-species sprite range bundled under {@code feed-icons/pokemon/}. */
    private static final int MAX_DEX = 1025;
    /** Placeholder stored for assets that failed to open/decode; never handed back to callers. */
    private static final Bitmap MISSING = Bitmap.createBitmap(1, 1, Bitmap.Config.ALPHA_8);

    private final Context context;
    private final LruCache<String, Bitmap> cache = new LruCache<String, Bitmap>(6 * 1024 * 1024) {
        @Override protected int sizeOf(String key, Bitmap value) {
            return value == MISSING ? 1 : value.getAllocationByteCount();
        }
    };

    public LiveFeedIcons(Context context) {
        Context app = context != null ? context.getApplicationContext() : null;
        this.context = app != null ? app : context;
    }

    /**
     * Bundled bitmap for an entry, or {@code null} when no suitable asset exists (the row then draws a
     * lettered fallback). Egg/boss state is read from the entry's hatch timestamp and confirmed-boss
     * flag, never from display text, so a stale egg row never shows a fabricated live boss.
     */
    public Bitmap iconFor(LiveFeedData.Entry e, long now) {
        if (e == null || e.kind == null) return null;
        switch (e.kind) {
            case POKEMON: return species(e.dexId);
            case STOP:    return mapIcon("stop.png");
            case GYM:     return mapIcon(teamIcon(e.team));
            case RAID:    return raidIcon(e, now);
            case STATION: return species(e.dexId); // a known power-spot boss uses its sprite, else fallback glyph
            case QUEST:   return e.dexId > 0 ? species(e.dexId) : mapIcon("stop.png"); // Pokémon reward sprite, else the stop
            default:      return null;
        }
    }

    private Bitmap raidIcon(LiveFeedData.Entry e, long now) {
        if (e.raidHatchAt > now) return mapIcon(eggIcon((int) Math.round(e.level))); // unhatched: show the egg
        if (e.raidBossLive) {
            Bitmap sprite = species(e.dexId);
            if (sprite != null) return sprite;
        }
        return mapIcon("raid-active.png"); // hatched but boss unconfirmed/missing — no predicted boss
    }

    private static String eggIcon(int level) {
        if (level == 5) return "raid-legendary.png";
        if (level == 3 || level == 4) return "raid-rare.png";
        return "raid-normal.png"; // unknown/special/low tier keeps the plain egg cue
    }

    private static String teamIcon(int team) {
        switch (team) {
            case 1: return "team-blue.png";
            case 2: return "team-red.png";
            case 3: return "team-yellow.png";
            default: return "gym.png"; // uncontested/unknown stays neutral, never a fabricated team colour
        }
    }

    private Bitmap species(int dexId) {
        if (dexId < 1 || dexId > MAX_DEX) return null;
        return decode("pgrazor/feed-icons/pokemon/" + dexId + ".png");
    }

    private Bitmap mapIcon(String name) {
        return decode("pgrazor/map-icons/" + name);
    }

    private Bitmap decode(String path) {
        Bitmap cached = cache.get(path);
        if (cached != null) return cached == MISSING ? null : cached;
        Bitmap bitmap = null;
        if (context != null) {
            try (InputStream in = context.getAssets().open(path)) {
                BitmapFactory.Options opts = new BitmapFactory.Options();
                opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
                bitmap = BitmapFactory.decodeStream(in, null, opts);
            } catch (Throwable ignored) { bitmap = null; }
        }
        cache.put(path, bitmap != null ? bitmap : MISSING);
        return bitmap;
    }
}
