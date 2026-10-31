import { useEffect, useRef, useState } from 'react';
import { nav, links } from '../content.js';
import { asset } from '../assets.js';
import { homeHref } from './guideData.js';
import { GithubIcon, SunIcon, MoonIcon, MenuIcon, CloseIcon } from './Icons.jsx';

const NAV_ID = 'primary-nav';

export default function Header({ theme, onToggleTheme, onGuide = false }) {
  const [open, setOpen] = useState(false);
  const toggleRef = useRef(null);
  const headerRef = useRef(null);

  // While the mobile menu is open, Escape closes it (returning focus to the
  // toggle) and a click outside the header dismisses it.
  useEffect(() => {
    if (!open) return undefined;

    const onKeyDown = (event) => {
      if (event.key === 'Escape') {
        setOpen(false);
        toggleRef.current?.focus();
      }
    };
    const onPointerDown = (event) => {
      if (headerRef.current && !headerRef.current.contains(event.target)) {
        setOpen(false);
      }
    };

    document.addEventListener('keydown', onKeyDown);
    document.addEventListener('pointerdown', onPointerDown);
    return () => {
      document.removeEventListener('keydown', onKeyDown);
      document.removeEventListener('pointerdown', onPointerDown);
    };
  }, [open]);

  const nextTheme = theme === 'dark' ? 'light' : 'dark';
  // On guide pages the in-page anchors don't exist, so point home-bound links at
  // the homepage (and its section anchors) instead of a dead fragment.
  const brandHref = onGuide ? homeHref : '#top';
  const anchorHref = (hash) => (onGuide ? `${homeHref}${hash}` : hash);

  return (
    <header className="site-header" ref={headerRef}>
      <div className="site-header__inner">
        <a className="brand" href={brandHref}>
          <img className="brand__mark" src={asset('logo.svg')} alt="" width="28" height="28" />
          <span className="brand__name">PGRazor</span>
        </a>

        <nav
          id={NAV_ID}
          className={`site-nav${open ? ' is-open' : ''}`}
          aria-label="Primary"
        >
          <ul className="site-nav__list">
            {nav.map((item) => (
              <li key={item.href}>
                <a href={anchorHref(item.href)} onClick={() => setOpen(false)}>
                  {item.label}
                </a>
              </li>
            ))}
          </ul>
          <a className="site-nav__github" href={links.github} onClick={() => setOpen(false)}>
            <GithubIcon />
            <span>GitHub</span>
          </a>
        </nav>

        <div className="site-header__actions">
          <button
            type="button"
            className="icon-btn"
            onClick={onToggleTheme}
            aria-label={`Switch to ${nextTheme} theme`}
            title={`Switch to ${nextTheme} theme`}
          >
            {theme === 'dark' ? <SunIcon /> : <MoonIcon />}
          </button>
          <button
            type="button"
            className="icon-btn nav-toggle"
            ref={toggleRef}
            aria-expanded={open}
            aria-controls={NAV_ID}
            aria-label={open ? 'Close menu' : 'Open menu'}
            onClick={() => setOpen((value) => !value)}
          >
            {open ? <CloseIcon /> : <MenuIcon />}
          </button>
        </div>
      </div>
    </header>
  );
}
