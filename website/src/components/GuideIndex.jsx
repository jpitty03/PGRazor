import { useMemo, useState } from 'react';
import { featureGuides, guideHref, homeHref } from './guideData.js';

// The feature-guide index: a short explainer, an optional text filter, and the
// guides grouped by their category. Every card is an ordinary link to
// ?guide=<id>, so sharing and the back button work without a client router.
export default function GuideIndex() {
  const [query, setQuery] = useState('');
  const term = query.trim().toLowerCase();

  const filtered = useMemo(() => {
    if (!term) return featureGuides;
    return featureGuides.filter((guide) =>
      [guide.title, guide.summary, guide.category, guide.availability]
        .filter(Boolean)
        .some((field) => String(field).toLowerCase().includes(term)),
    );
  }, [term]);

  // Preserve the order categories first appear in the catalog.
  const categories = useMemo(() => {
    const seen = [];
    for (const guide of filtered) {
      if (guide.category && !seen.includes(guide.category)) seen.push(guide.category);
    }
    return seen;
  }, [filtered]);

  const empty = featureGuides.length === 0;

  return (
    <section className="section guide-index" aria-labelledby="guide-index-heading">
      <div className="section__head">
        <p className="section__num">Guides</p>
        <h1 id="guide-index-heading" className="section__title">
          Feature guides
        </h1>
        <p className="section__lead">
          Step-by-step how-tos for PGRazor’s catching, location, automation, and
          personalisation features. Open any guide for where to find the option and how to use it.
        </p>
      </div>

      <p className="guide-note">
        Each guide shows its <strong>availability</strong>. Some newer guides describe the current
        development build, which is not yet in a published release — the latest release APK does
        not necessarily include every feature documented here. Nothing on this page is a promise
        about a future release.
      </p>

      {empty ? (
        <p className="guide-empty">
          The feature guides are being prepared. In the meantime,{' '}
          <a href={homeHref}>return to the homepage</a>.
        </p>
      ) : (
        <>
          <form className="guide-search" role="search" onSubmit={(event) => event.preventDefault()}>
            <label htmlFor="guide-search-input">Search guides</label>
            <input
              id="guide-search-input"
              type="search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder="Search by name or topic"
              autoComplete="off"
            />
          </form>

          <p className="visually-hidden" role="status" aria-live="polite">
            {filtered.length} {filtered.length === 1 ? 'guide' : 'guides'}
            {term ? ` matching “${query.trim()}”` : ' available'}.
          </p>

          {filtered.length === 0 ? (
            <p className="guide-empty">
              No guides match “{query.trim()}”.{' '}
              <button type="button" onClick={() => setQuery('')}>
                Clear search
              </button>
            </p>
          ) : (
            categories.map((category) => {
              const headingId = `guide-group-${category.replace(/[^a-z0-9]+/gi, '-').toLowerCase()}`;
              return (
                <section className="guide-group" key={category} aria-labelledby={headingId}>
                  <h2 id={headingId} className="guide-group__title">
                    {category}
                  </h2>
                  <ul className="guide-cards">
                    {filtered
                      .filter((guide) => guide.category === category)
                      .map((guide) => (
                        <li key={guide.id}>
                          <a className="guide-card" href={guideHref(guide.id)}>
                            <span className="guide-card__title">{guide.title}</span>
                            <span className="guide-card__summary">{guide.summary}</span>
                            {guide.availability && (
                              <span className="guide-card__tag">
                                <span className="guide-card__tag-label">Availability</span>
                                {guide.availability}
                              </span>
                            )}
                          </a>
                        </li>
                      ))}
                  </ul>
                </section>
              );
            })
          )}
        </>
      )}

      <p className="guide-back">
        <a href={homeHref}>← Back to homepage</a>
      </p>
    </section>
  );
}
