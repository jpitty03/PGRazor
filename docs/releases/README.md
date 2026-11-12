# Release-note format

Use this format for every release, including experimental releases. Copy [TEMPLATE.md](TEMPLATE.md) to `docs/releases/vVERSION.md` and replace all placeholders.

- **New features:** explain each feature in one or two short sentences.
- **Improvements & fixes:** use short bullets describing the result. Group minor fixes or UI improvements when useful; omit empty sections.
- No verification section, test counts, build details, or website-only changes. Keep verification and diagnostic evidence in internal receipts/handoffs. Keep CHANGELOG.md concise too, and omit website-only entries there.
- Avoid unsupported success claims. Include a brief limitation with the relevant feature only when users need it to understand the feature.
- Always include release-specific links for the non-root APK, Root installer APK, Root module ZIP, and GitHub release. Keep the optional Ko-fi support link and GitHub repository link in [footer.md](footer.md).

The release workflow publishes the versioned notes followed by `footer.md`. Discord reads that same GitHub release body, so write it once; do not maintain a separate verbose Discord version. Discord also adds download fields for all three artifacts and links its title to the GitHub release.

For manual publication, append the footer exactly once, just as the workflow does. Review the rendered notes and links before publishing. Do not repost an existing Discord announcement just to change formatting.
