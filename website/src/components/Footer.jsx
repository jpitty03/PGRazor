import { links } from '../content.js';
import { asset } from '../assets.js';
import { homeHref, indexHref } from './guideData.js';

export default function Footer({ onGuide = false }) {
  return (
    <footer className="site-footer">
      <div className="site-footer__inner">
        <div className="site-footer__top">
          <a className="brand" href={onGuide ? homeHref : '#top'}>
            <img className="brand__mark" src={asset('logo.svg')} alt="" width="26" height="26" />
            <span className="brand__name">PGRazor</span>
          </a>
          <nav className="site-footer__links" aria-label="Footer">
            <a href={indexHref}>Feature guides</a>
            <a href={links.github}>GitHub</a>
            <a href={links.releasesLatest}>Releases</a>
            <a href={links.discord}>Discord</a>
            <a href={links.kofi}>Ko-fi</a>
            <a href={`${links.github}/blob/main/THIRD_PARTY.md`}>Credits</a>
          </nav>
        </div>
        <p className="site-footer__legal">
          Original PGRazor source is released under the MIT License; bundled third-party components
          keep their own licenses. PGRazor is an independent, open-source project and is not
          affiliated with the publisher of Pokémon GO. Pokémon and its assets belong to their
          respective owners.
        </p>
        <p className="site-footer__legal">Map screenshot © <a href="https://www.openstreetmap.org/copyright">OpenStreetMap contributors</a>.</p>
      </div>
    </footer>
  );
}
