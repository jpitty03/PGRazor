import { useEffect, useMemo } from 'react';
import Header from './components/Header.jsx';
import Hero from './components/Hero.jsx';
import Features from './components/Features.jsx';
import Cooldown from './components/Cooldown.jsx';
import GetStarted from './components/GetStarted.jsx';
import Footer from './components/Footer.jsx';
import GuideIndex from './components/GuideIndex.jsx';
import GuideDetail from './components/GuideDetail.jsx';
import { featureGuides } from './components/guideData.js';
import { useTheme } from './hooks/useTheme.js';

const HOME_TITLE = 'PGRazor — smoother Pokémon GO sessions';
const HOME_DESCRIPTION =
  'PGRazor is a compact, open-source Android interface for Pokémon GO: location controls, catching helpers, and optional automation. One APK for ARM64 Android phones and the MuMu emulator, no root required.';

// Query-string routing for a static GitHub Pages build (base /PGRazor/):
//   ?guide=all  → the feature-guide index
//   ?guide=<id> → one feature guide
//   (no query)  → the marketing homepage
// Each guide link is an ordinary <a> that changes the query, so the browser
// does a full load of the same index.html. Reload, deep links, sharing, and the
// back button all work without a client-side router, which keeps the static
// Pages deploy dependency-free.
function readRoute() {
  if (typeof window === 'undefined') return { view: 'home' };
  const guide = new URLSearchParams(window.location.search).get('guide');
  if (guide === 'all') return { view: 'index' };
  if (guide) return { view: 'detail', id: guide };
  return { view: 'home' };
}

export default function App() {
  const { theme, toggle } = useTheme();
  const route = useMemo(readRoute, []);
  const guide =
    route.view === 'detail' ? featureGuides.find((item) => item.id === route.id) : null;
  const onGuide = route.view !== 'home';

  // Keep the document title and meta description in step with the view so a
  // shared or bookmarked guide link describes the guide, not just the homepage.
  useEffect(() => {
    const meta = document.querySelector('meta[name="description"]');
    let title = HOME_TITLE;
    let description = HOME_DESCRIPTION;
    if (route.view === 'index') {
      title = 'Feature guides — PGRazor';
      description =
        'Step-by-step how-to guides for PGRazor’s catching, location, automation, and personalisation features.';
    } else if (route.view === 'detail' && guide) {
      title = `${guide.title} — PGRazor feature guide`;
      description = guide.summary || description;
    } else if (route.view === 'detail') {
      title = 'Guide not found — PGRazor';
      description =
        'That PGRazor feature guide could not be found. Browse the full list of feature guides.';
    }
    document.title = title;
    if (meta && description) meta.content = description;
    const canonical = document.querySelector('link[rel="canonical"]');
    if (canonical) {
      const home = canonical.href.split('?')[0];
      canonical.href = route.view === 'home' ? home
        : `${home}?guide=${guide ? encodeURIComponent(guide.id) : 'all'}`;
      const url = document.querySelector('meta[property="og:url"]');
      if (url) url.content = canonical.href;
    }
    const ogTitle = document.querySelector('meta[property="og:title"]');
    const ogDescription = document.querySelector('meta[property="og:description"]');
    if (ogTitle) ogTitle.content = title;
    if (ogDescription) ogDescription.content = description;
  }, [route, guide]);

  return (
    <>
      <a className="skip-link" href="#main">Skip to content</a>
      <Header theme={theme} onToggleTheme={toggle} onGuide={onGuide} />
      <main id="main" tabIndex={-1}>
        {route.view === 'home' && (
          <>
            <Hero />
            <Features />
            <Cooldown />
            <GetStarted />
          </>
        )}
        {route.view === 'index' && <GuideIndex />}
        {route.view === 'detail' && <GuideDetail id={route.id} />}
      </main>
      <Footer onGuide={onGuide} />
    </>
  );
}
