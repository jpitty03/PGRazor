import SettingsGuide from './SettingsGuide.jsx';
import Gallery from './Gallery.jsx';
import { indexHref } from './guideData.js';

export default function Features() {
  return (
    <section className="section" id="features" aria-labelledby="features-heading">
      <div className="section__head">
        <p className="section__num">01 / Features</p>
        <h2 id="features-heading" className="section__title">
          Features.
        </h2>
        <p className="section__lead">
          Explore the settings guide, then take a closer look at the actual app screens.
        </p>
      </div>

      <p className="guide-note">These guides cover the experimental and stable releases. Check each guide's availability against your installed APK.</p>
      <p className="guide-back"><a className="btn btn--primary" href={indexHref}>Browse all feature guides</a></p>
      <SettingsGuide />
      <Gallery />
    </section>
  );
}
