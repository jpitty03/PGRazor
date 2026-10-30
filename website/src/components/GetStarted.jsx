import { steps, faqs, links } from '../content.js';
import { ArrowIcon } from './Icons.jsx';

export default function GetStarted() {
  return (
    <section className="section" id="get-started" aria-labelledby="get-started-heading">
      <div className="section__head">
        <p className="section__num">04 / Get started</p>
        <h2 id="get-started-heading" className="section__title">
          Three steps to your first session.
        </h2>
        <p className="section__lead">
          Every release ships with a changelog, a SHA-256 checksum, and build information.
        </p>
      </div>

      <ol className="steps">
        {steps.map((step) => (
          <li className="step" key={step.n}>
            <span className="step__num" aria-hidden="true">{step.n}</span>
            <h3 className="step__title">{step.title}</h3>
            <p className="step__body">{step.body}</p>
          </li>
        ))}
      </ol>

      <div className="cta">
        <a className="btn btn--primary" href={links.releasesLatest}>
          Get the latest release
          <ArrowIcon />
        </a>
        <a className="btn btn--ghost" href={links.discord}>
          Join the Discord
        </a>
      </div>

      <p className="cta__experimental">
        <span className="cta__badge">Experimental</span>
        Want the Events update early? The{' '}
        <a href={links.releasesExperimental}>v0.1.7 experimental prerelease</a> adds the
        Halloween box opener and the native runtime. It passed automated checks and MuMu
        testing, but hasn’t been retested on a physical phone — expect rough edges.
      </p>

      <div className="faq">
        <h3 className="faq__heading">Good to know</h3>
        {faqs.map((item) => (
          <details className="faq__item" key={item.q}>
            <summary>
              <span>{item.q}</span>
              <span className="faq__mark" aria-hidden="true" />
            </summary>
            <p>{item.a}</p>
          </details>
        ))}
      </div>
    </section>
  );
}
