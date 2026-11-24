package dev.pgrazor;

public final class UpdateRulesTest {
    private static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    public static void main(String[] args) {
        check(UpdateRules.newer("v0.3.7", "v0.3.6"), "patch release is newer");
        check(UpdateRules.newer("v0.4.0", "v0.3.9"), "minor release is newer");
        check(UpdateRules.newer("v1.0.0", "v0.9.9"), "major release is newer");
        check(UpdateRules.newer("v0.3.10", "v0.3.9"), "numbers compare as numbers, not text");
        check(!UpdateRules.newer("v0.3.6", "v0.3.6"), "same release is not newer");
        check(!UpdateRules.newer("v0.3.5", "v0.3.6"), "older release is not newer");
        check(UpdateRules.newer("v0.3.6", "v0.3.6-beta.1"), "a full release replaces its pre-release");
        check(!UpdateRules.newer("v0.3.6-beta.2", "v0.3.6"), "a pre-release does not replace the full release");
        check(!UpdateRules.newer("v0.3.6-beta.2", "v0.3.6-beta.1"), "pre-releases of one version are not ordered");
        check(!UpdateRules.newer("latest", "v0.3.6"), "an unreadable tag is never newer");
        check(!UpdateRules.newer("v0.3.7", "dev"), "an unreadable installed version never updates");
        check(!UpdateRules.newer(null, "v0.3.6"), "a missing tag is never newer");
        check(UpdateRules.parse("0.3") == null && UpdateRules.parse("v0.3.6.1") == null && UpdateRules.parse("v0..6") == null, "malformed versions are rejected");
        check(UpdateRules.parse("v1234567.0.0") == null, "over-long numbers are rejected");

        String hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF";
        String list = hash + "  PGRazor.apk\n" + "ff" + hash.substring(2) + " *PGRazor-Root.apk\n";
        check(hash.toLowerCase(java.util.Locale.US).equals(UpdateRules.checksum(list, "PGRazor.apk")), "checksum is found and lower-cased");
        check(UpdateRules.checksum(list, "PGRazor-Root.apk").startsWith("ff"), "a starred name is accepted");
        check(UpdateRules.checksum(list, "PGRazor-Root-module.zip") == null, "an unlisted file has no checksum");
        check(UpdateRules.checksum(list + hash + "  PGRazor.apk\n", "PGRazor.apk") == null, "a file listed twice is rejected");
        check(UpdateRules.checksum("zz" + hash.substring(2) + "  PGRazor.apk\n", "PGRazor.apk") == null, "a non-hex checksum is rejected");
        check(UpdateRules.checksum(hash.substring(1) + "  PGRazor.apk\n", "PGRazor.apk") == null, "a short checksum is rejected");

        check(UpdateRules.trustedUrl("https://github.com/jpitty03/PGRazor/releases/download/v0.3.7/PGRazor.apk"), "the release URL is trusted");
        check(!UpdateRules.trustedUrl("http://github.com/jpitty03/PGRazor/releases/download/v0.3.7/PGRazor.apk"), "plain HTTP is refused");
        check(!UpdateRules.trustedUrl("https://github.com/someone/PGRazor/releases/download/v0.3.7/PGRazor.apk"), "another repository is refused");
        check(!UpdateRules.trustedUrl(null), "a missing URL is refused");

        check(UpdateRules.sizeAllowed(186_207_943L) && !UpdateRules.sizeAllowed(0) && !UpdateRules.sizeAllowed(UpdateRules.MAX_BYTES + 1), "sizes are bounded");
        check("178 MB".equals(UpdateRules.megabytes(186_207_943L)) && "".equals(UpdateRules.megabytes(0)), "size label");
        check("PGRazor.apk".equals(UpdateRules.assetName(false)) && "PGRazor-Root.apk".equals(UpdateRules.assetName(true)), "asset per build");
        System.out.println("UpdateRulesTest passed");
    }
}
