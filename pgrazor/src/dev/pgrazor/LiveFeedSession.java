package dev.pgrazor;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * App-private persistence for the PoGoCoordinates session token/cookies. The token is encrypted with an
 * AES/GCM key held in the AndroidKeyStore and stored in a {@code MODE_PRIVATE} SharedPreferences
 * file; the password is never persisted and the token never appears in logs or exceptions.
 *
 * <p>The constructor does no crypto so it is safe on the main thread. Key access and (de)cryption
 * happen lazily on {@link #token()} / {@link #save(String)}, which callers must invoke off the UI
 * thread. {@link #signedIn()} only inspects for the presence of a stored blob and stays cheap.
 */
public final class LiveFeedSession {
    private static final String PREFS = "pgrazor-livefeed-v1";
    private static final String KEY_ALIAS = "pgrazor_livefeed_token_v1";
    private static final String KEY_CIPHERTEXT = "token_ct";
    private static final String KEY_IV = "token_iv";
    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final int GCM_TAG_BITS = 128;

    private final SharedPreferences preferences;
    private volatile String cachedToken; // decrypted token held only in memory once resolved

    public LiveFeedSession(Context context) {
        preferences = context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** Cheap presence check; does not touch the keystore so it is safe to call on the main thread. */
    public boolean signedIn() {
        return cachedToken != null || (preferences.contains(KEY_CIPHERTEXT) && preferences.contains(KEY_IV));
    }

    /**
     * Returns the decrypted bearer token, or {@code null} if none is stored or it cannot be
     * recovered (in which case the corrupt blob is cleared). Decrypts lazily; call off the UI thread.
     */
    public synchronized String token() {
        String token = cachedToken;
        if (token != null) return token;
        String ciphertext = preferences.getString(KEY_CIPHERTEXT, null);
        String iv = preferences.getString(KEY_IV, null);
        if (ciphertext == null || iv == null) return null;
        try {
            Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.DECRYPT_MODE, loadKey(),
                    new GCMParameterSpec(GCM_TAG_BITS, Base64.decode(iv, Base64.NO_WRAP)));
            byte[] plain = cipher.doFinal(Base64.decode(ciphertext, Base64.NO_WRAP));
            token = new String(plain, StandardCharsets.UTF_8);
            cachedToken = token;
            return token;
        } catch (Exception e) {
            // Key rotated/lost or blob tampered with: drop it and require a fresh sign-in.
            clear();
            return null;
        }
    }

    /** Encrypts and persists the token. Throws on storage/crypto failure. Call off the UI thread. */
    public synchronized void save(String token) throws Exception {
        if (token == null || token.isEmpty()) throw new IllegalArgumentException("Empty session token.");
        Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        cipher.init(Cipher.ENCRYPT_MODE, loadOrCreateKey());
        byte[] iv = cipher.getIV();
        byte[] ciphertext = cipher.doFinal(token.getBytes(StandardCharsets.UTF_8));
        boolean ok = preferences.edit()
                .putString(KEY_CIPHERTEXT, Base64.encodeToString(ciphertext, Base64.NO_WRAP))
                .putString(KEY_IV, Base64.encodeToString(iv, Base64.NO_WRAP))
                .commit();
        if (!ok) throw new java.io.IOException("Could not store the session. Please retry.");
        cachedToken = token;
    }

    /** Forgets the token (sign-out). Leaves the keystore key in place for reuse. */
    public synchronized void clear() {
        cachedToken = null;
        preferences.edit().remove(KEY_CIPHERTEXT).remove(KEY_IV).apply();
    }

    private SecretKey loadKey() throws Exception {
        KeyStore keyStore = KeyStore.getInstance(KEYSTORE);
        keyStore.load(null);
        KeyStore.Entry entry = keyStore.getEntry(KEY_ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        throw new IllegalStateException("Missing session key.");
    }

    private SecretKey loadOrCreateKey() throws Exception {
        KeyStore keyStore = KeyStore.getInstance(KEYSTORE);
        keyStore.load(null);
        if (keyStore.containsAlias(KEY_ALIAS)) {
            KeyStore.Entry entry = keyStore.getEntry(KEY_ALIAS, null);
            if (entry instanceof KeyStore.SecretKeyEntry) return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        }
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build());
        return generator.generateKey();
    }
}
