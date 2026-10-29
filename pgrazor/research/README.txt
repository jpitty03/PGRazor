FRIDA RESEARCH TOOLS ONLY

Production builds use the compiled native/Java runtime. Frida is retained for explicitly selected research builds (--research-runtime), never as a production fallback. Keep research installs on the authorized MuMu; no physical phone operation is implied.

Retained entry points in ../diagnostics: il2cpp-survey.js, camera-probe.js, scene-probe.js, interaction-probe.js and nativebridge probes. ../embedded-agent.js supplies research bootstrap/location/auth/transport; ../runtime/bundles.json is the complete ordered research bundle manifest. ../vendor contains the required Frida Java bridge, its patch/provenance and licenses. These files are intentionally identifiable and not production APK assets.

Native schema inputs live at ../native/schema/routes.json and gifts.json. Update these validated descriptors after research, regenerate the corresponding headers using tools/native_routes_schema.py and tools/native_gift_schema.py, and verify actual native fixtures. Production builds must not extract metadata from research JavaScript.

Retired: JavaScript quick-close, quick-ball, quick-summary gameplay, runtime/features factories, and transitional adapters outside the retained research closure. Their native C++/Java replacements and native fixtures remain. Legacy adapter-only tests were retired with their implementations; retained research Node tests still run via tools/check.py.

Diagnostic results establish hypotheses, not native correctness. Check method ABI, thread ownership, rooted-object lifetimes, stale/revoked settings and failure paths in the compiled implementation before packaging production. Compilation does not make code unrecoverable or embedded secrets safe.
