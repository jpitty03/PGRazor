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

## Feature guides and routing

The site has three views selected by a query string, so the static GitHub Pages deploy needs no client-side router and every link survives reload, sharing, and the back button:

- no query — the marketing homepage (`Hero`, `Features`, `Cooldown`, `GetStarted`).
- `?guide=all` — the feature-guide index (`GuideIndex`): the guides grouped by `category` with a text filter.
- `?guide=<id>` — one guide (`GuideDetail`). An unknown id renders an in-page "Guide not found" with a link back to the index.

`App.jsx` reads `window.location.search` once on load, picks the view, and updates `document.title` and the `meta[name="description"]` to match. Guide links are ordinary `<a>` elements whose `href` changes the query, which triggers a full load of the same `index.html`. On guide views the header/footer logo and nav point back at the homepage and its section anchors (`/PGRazor/#features`, …) instead of dead in-page fragments.

Guide content lives in `src/featureGuides.js`, which exports `featureGuides` — an array of `{ id, title, category, summary, availability, where, steps[], notes[] }`. That file is maintained separately from the layout. `src/components/guideData.js` re-exports the catalog and provides base-aware links. To add or edit a guide, update `featureGuides.js`; the index and detail pages pick it up without component edits. The homepage Features section links to the guide index.

Each guide's `availability` is shown prominently (an index tag and a callout on the detail page). Features documented only in the current development build are not promised in, or implied to be part of, the latest release APK; keep `availability` accurate and avoid release promises.

## Screenshots and artwork

- `public/logo.svg`: original PGRazor branding, shared with `pgrazor/branding/pgrazor.svg`.
- `public/images/main-menu.png`: canonical app settings screenshot, also referenced by the repository README.
- `public/images/encounter.png`: real encounter capture showing the IV/level, transfer and berry overlays.
- `public/images/world-map.png`: October 2026 MuMu capture of the app's world map. OpenStreetMap attribution is visible in the capture.

Screenshots are representative; enabled options and layout depend on your settings and build. Do not publish account names, email addresses, tokens, or private locations in new captures. Game artwork is not covered by the source MIT license; see the repository's [third-party notices](../THIRD_PARTY.md). Map data is © [OpenStreetMap contributors](https://www.openstreetmap.org/copyright).

## Cooldown guidance

The Cooldown section uses original text and two compact tables with all 30 estimates visible without internal scrolling. The values are guidance, not a published server contract. The app interpolates between distances and rounds up to whole minutes. Preserve the chart values and action exceptions when adjusting presentation. Published release downloads remain separate from development-build guides.

## Guide screenshots

Feature pages use `src/guideScreenshots.js` and the shared `Gallery` component. The 11 WebP images under `public/images/guides/` were captured from the released experimental.3 APK on the research MuMu on 2026-10-31, then losslessly cropped to the relevant panel. No controls or results were synthesized. Captions identify example settings rather than defaults or proof of successful gameplay; the map example selects Pier 39 without moving the character. OpenStreetMap attribution remains visible.

Keep captures free of account names, saved places, private coordinates and unrelated overlays. Original captures and crop receipts remain in local diagnostic artifacts; publish only the reviewed panel images. The image catalog records dimensions for stable layout. Guide galleries support keyboard opening, Escape, a close button and focus restoration.
