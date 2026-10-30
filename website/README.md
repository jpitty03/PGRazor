# PGRazor website

The public app showcase lives at [jpitty03.github.io/PGRazor](https://jpitty03.github.io/PGRazor/). It is a static React site built with Vite. The Android app and APK release workflows are independent of this site.

## Local development

Use Node 24 and npm:

```sh
cd website
npm ci
npm run dev
```

Open the printed local URL with the `/PGRazor/` path. To verify the production build:

```sh
npm run build
npx playwright install chromium
npm test
```

The browser checks use the production preview server. Generated output, installed dependencies, and test receipts are ignored.

## Publishing

The `Website` workflow builds and checks pull requests. Changes to `website/` or the workflow on `main` also deploy to GitHub Pages. Repository Pages must use **GitHub Actions** as its source. Only the deployment job receives Pages write and OIDC permissions. It cannot publish APK releases or send release announcements.

The Vite base path is `/PGRazor/`. Keep image links base-aware when adding screenshots. Download buttons point to GitHub's `/releases/latest` so publishing an APK does not require a website edit.

## Screenshots and artwork

- `public/logo.svg`: original PGRazor branding, shared with `pgrazor/branding/pgrazor.svg`.
- `public/images/main-menu.png`: canonical app settings screenshot, also referenced by the repository README.
- `public/images/encounter.png`: real encounter capture showing the IV/level, transfer and berry overlays.
- `public/images/world-map.png`: October 2026 MuMu capture of the app's world map. OpenStreetMap attribution is visible in the capture.

Screenshots are representative; enabled options and layout depend on your settings and build. Do not publish account names, email addresses, tokens, or private locations in new captures. Game artwork is not covered by the source MIT license; see the repository's [third-party notices](../THIRD_PARTY.md). Map data is © [OpenStreetMap contributors](https://www.openstreetmap.org/copyright).

## Cooldown guidance

The Cooldown section uses original text and an HTML table, with a reference link to the community chart. It is guidance, not a published server contract. The listed values match the reference chart used by the app; the app interpolates between distances and rounds up to whole minutes. Review action exceptions when updating the guide. No competitor artwork is bundled. The stable download remains separate from the experimental prerelease link.
