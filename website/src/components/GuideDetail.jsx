import { featureGuides, guideHref, homeHref, indexHref } from './guideData.js';
import Gallery from './Gallery.jsx';
import { guideScreenshots } from '../guideScreenshots.js';

// One feature's how-to page. An unknown id (renamed, removed, or mistyped in a
// shared link) renders a useful not-found view with a route back to the index
// rather than a blank page.
export default function GuideDetail({ id }) {
  const guide = featureGuides.find((item) => item.id === id);

  if (!guide) {
    return (
      <section className="section guide-detail" aria-labelledby="guide-notfound-heading">
        <div className="section__head">
          <p className="section__num">Guides</p>
          <h1 id="guide-notfound-heading" className="section__title">
            Guide not found
          </h1>
          <p className="section__lead">
            We couldn’t find a feature guide for “{id}”. It may have been renamed or removed, or the
            link may be incomplete.
          </p>
        </div>
        <p className="guide-back">
          <a className="btn btn--primary" href={indexHref}>
            Browse all feature guides
          </a>
          <a className="btn btn--ghost" href={homeHref}>
            Back to homepage
          </a>
        </p>
      </section>
    );
  }

  const steps = Array.isArray(guide.steps) ? guide.steps : [];
  const notes = Array.isArray(guide.notes) ? guide.notes : [];
  const related = featureGuides.filter(
    (item) => item.category === guide.category && item.id !== guide.id,
  );

  return (
    <article className="section guide-detail" aria-labelledby="guide-detail-heading">
      <nav className="guide-detail__crumbs" aria-label="Breadcrumb">
        <a href={homeHref}>Home</a>
        <span aria-hidden="true">/</span>
        <a href={indexHref}>Feature guides</a>
        <span aria-hidden="true">/</span>
        <span aria-current="page">{guide.title}</span>
      </nav>

      <div className="section__head">
        {guide.category && <p className="section__num">{guide.category}</p>}
        <h1 id="guide-detail-heading" className="section__title">
          {guide.title}
        </h1>
        {guide.summary && <p className="section__lead">{guide.summary}</p>}
      </div>

      {guide.availability && (
        <p className="guide-detail__availability" role="note">
          <span className="guide-detail__availability-label">Availability</span>
          <span>{guide.availability}</span>
        </p>
      )}

      {guide.where && (
        <section className="guide-detail__block" aria-labelledby="guide-where-heading">
          <h2 id="guide-where-heading">Where to find it</h2>
          <p>{guide.where}</p>
        </section>
      )}

      {steps.length > 0 && (
        <section className="guide-detail__block" aria-labelledby="guide-steps-heading">
          <h2 id="guide-steps-heading">Step by step</h2>
          <ol className="guide-steps">
            {steps.map((step, index) => (
              <li key={index}>{step}</li>
            ))}
          </ol>
        </section>
      )}

      {guideScreenshots[id]?.length > 0 && (
        <section className="guide-detail__block guide-screenshots" aria-labelledby="guide-screenshots-heading">
          <h2 id="guide-screenshots-heading">Screenshots</h2>
          <p>Captured on MuMu. These examples show configured settings, not necessarily the defaults.</p>
          <Gallery shots={guideScreenshots[id]} />
        </section>
      )}

      {notes.length > 0 && (
        <section className="guide-detail__block" aria-labelledby="guide-notes-heading">
          <h2 id="guide-notes-heading">Good to know</h2>
          <ul className="guide-notes">
            {notes.map((note, index) => (
              <li key={index}>{note}</li>
            ))}
          </ul>
        </section>
      )}

      <nav className="guide-detail__related" aria-label="More guides">
        <h2>More guides</h2>
        {related.length > 0 && (
          <ul className="guide-related">
            {related.map((item) => (
              <li key={item.id}>
                <a href={guideHref(item.id)}>{item.title}</a>
              </li>
            ))}
          </ul>
        )}
        <p className="guide-back">
          <a className="btn btn--ghost" href={indexHref}>
            All feature guides
          </a>
          <a className="btn btn--ghost" href={homeHref}>
            Back to homepage
          </a>
        </p>
      </nav>
    </article>
  );
}
