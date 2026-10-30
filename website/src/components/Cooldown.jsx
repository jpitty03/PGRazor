import { cooldown, links } from '../content.js';

// Community cooldown guidance: a short explainer, two expandable action lists,
// and a scrollable distance/wait table. All values are framed as estimates.
export default function Cooldown() {
  return (
    <section className="section" id="cooldown" aria-labelledby="cooldown-heading">
      <div className="section__head">
        <p className="section__num">03 / Cooldown</p>
        <h2 id="cooldown-heading" className="section__title">
          Cooldown between locations.
        </h2>
        <p className="section__lead">{cooldown.intro}</p>
      </div>

      <div className="cooldown__groups">
        <details className="cooldown__group" open>
          <summary>
            <span>Starts the cooldown</span>
            <span className="faq__mark" aria-hidden="true" />
          </summary>
          <ul className="cooldown__list cooldown__list--trigger">
            {cooldown.triggers.map((item) => (
              <li key={item}>{item}</li>
            ))}
          </ul>
        </details>

        <details className="cooldown__group">
          <summary>
            <span>Does not start it</span>
            <span className="faq__mark" aria-hidden="true" />
          </summary>
          <ul className="cooldown__list cooldown__list--safe">
            {cooldown.safe.map((item) => (
              <li key={item}>{item}</li>
            ))}
          </ul>
        </details>
      </div>

      <figure className="cooldown__chart">
        <figcaption className="cooldown__chart-cap">
          Community-reported wait between action locations. These are estimates — when in
          doubt, round up to the next listed distance.
        </figcaption>
        <div className="cooldown__table-wrap" tabIndex={0} role="region" aria-label="Distance and wait estimates">
          <table className="cooldown__table">
            <caption className="visually-hidden">
              Distance between actions in kilometres and the community-reported cooldown wait, as estimates.
            </caption>
            <thead>
              <tr>
                <th scope="col">Distance (km)</th>
                <th scope="col">Reported wait (est.)</th>
              </tr>
            </thead>
            <tbody>
              {cooldown.chart.map((row) => (
                <tr key={row.km}>
                  <th scope="row">{row.km}</th>
                  <td>{row.wait}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </figure>

      <p className="cooldown__note">
        Community guidance and estimates, not official rules — no promise of safety or
        zero bans. PGRazor’s on-screen timer is a local estimate, not server state.{' '}
        <a href={links.cooldownReference} rel="nofollow noopener">
          PGSharp reference
        </a>{' '}
        last reviewed {cooldown.reviewed}.
      </p>
    </section>
  );
}
