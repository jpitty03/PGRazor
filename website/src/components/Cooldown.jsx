import { cooldown } from '../content.js';

// Community cooldown guidance: a short explainer, two expandable action lists,
// and a compact distance/wait tables. All values are framed as estimates.
export default function Cooldown() {
  return (
    <section className="section" id="cooldown" aria-labelledby="cooldown-heading">
      <div className="section__head">
        <p className="section__num">02 / Cooldown</p>
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
          Distance between actions and estimated wait. Read down each column.
        </figcaption>
        <div className="cooldown__tables">
          {[cooldown.chart.slice(0, 15), cooldown.chart.slice(15)].map((rows, index) => (
            <div className="cooldown__table-wrap" key={index}>
              <table className="cooldown__table">
                <caption className="visually-hidden">Distance and estimated wait, part {index + 1} of 2. Read each table from top to bottom.</caption>
                <thead><tr><th scope="col">Distance (km)</th><th scope="col">Wait (est.)</th></tr></thead>
                <tbody>{rows.map(row => (
                  <tr key={row.km}><th scope="row">{row.km}</th><td>{row.wait}</td></tr>
                ))}</tbody>
              </table>
            </div>
          ))}
        </div>
      </figure>

      <p className="cooldown__note">
        PGRazor's timer is a local estimate, not a server countdown. Waiting does not
        guarantee that an action will succeed.
      </p>
    </section>
  );
}
