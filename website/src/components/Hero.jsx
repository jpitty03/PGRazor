import { links, heroFacts } from '../content.js';
import { asset } from '../assets.js';
import { ArrowIcon } from './Icons.jsx';
import RouteMotif from './RouteMotif.jsx';

export default function Hero() {
  return (
    <section className="hero" id="top">
      <RouteMotif className="hero__motif" />
      <div className="hero__inner">
        <div className="hero__copy">
          <p className="eyebrow">
            <span className="eyebrow__dot" aria-hidden="true" />
            Independent, open-source project
          </p>
          <h1 className="hero__title">
            Smoother sessions.{' '}
            <span className="accent-text">One small menu.</span>
          </h1>
          <p className="hero__lead">
            Location controls, faster catches, and optional automation for Pokémon GO.
            One APK for ARM64 Android and the MuMu emulator, with no root required.
          </p>
          <div className="hero__actions">
            <a className="btn btn--primary" href={links.releasesLatest}>
              Download latest release
              <ArrowIcon />
            </a>
            <a className="btn btn--ghost" href={links.github}>
              View source on GitHub
            </a>
          </div>
          <dl className="hero__facts">
            {heroFacts.map((fact) => (
              <div className="hero__fact" key={fact.term}>
                <dt>{fact.term}</dt>
                <dd>{fact.detail}</dd>
              </div>
            ))}
          </dl>
        </div>

        <figure className="hero__shot">
          <img
            src={asset('images/encounter.png')}
            alt="PGRazor overlays during a Pokémon GO encounter: a CP, level and IV read-out above the Pokémon, a berry count, a transfer shield, and the VGP status indicator."
            width="1080"
            height="2404"
            loading="eager"
            fetchPriority="high"
            decoding="async"
          />
          <figcaption className="shot-tag">
            <span className="shot-tag__num" aria-hidden="true">01</span>
            Overlays drawn over a live encounter
          </figcaption>
        </figure>
      </div>
    </section>
  );
}
