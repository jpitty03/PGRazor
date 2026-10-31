import { useEffect, useRef, useState } from 'react';
import { screenshots } from '../content.js';
import { asset } from '../assets.js';
import { ExpandIcon, CloseIcon } from './Icons.jsx';

// Screenshots open in a native <dialog>: the element handles modal focus
// trapping, Escape-to-close, and restoring focus to the trigger on close.
export default function Gallery({ shots = screenshots }) {
  const dialogRef = useRef(null);
  const [active, setActive] = useState(null);

  // Clear state on any native close (Escape, close button, backdrop) so the
  // same screenshot can be reopened. Done imperatively to not depend on the
  // React `close` event being registered.
  useEffect(() => {
    const dialog = dialogRef.current;
    if (!dialog) return undefined;
    const handleClose = () => setActive(null);
    dialog.addEventListener('close', handleClose);
    return () => dialog.removeEventListener('close', handleClose);
  }, []);

  useEffect(() => {
    const dialog = dialogRef.current;
    if (active && dialog && !dialog.open) {
      dialog.showModal();
    }
  }, [active]);

  const close = () => dialogRef.current?.close();

  // Backdrop clicks land on the dialog element itself, not the inner content.
  const onDialogClick = (event) => {
    if (event.target === dialogRef.current) close();
  };

  return (
    <div className="gallery">
      <p className="gallery__hint">Tap a screenshot for a closer look.</p>
      <ul className="gallery__grid">
        {shots.map((shot) => (
          <li key={shot.id}>
            <button type="button" className="shot-btn" onClick={() => setActive(shot)}>
              <img
                src={asset(shot.src)}
                alt=""
                width={shot.width}
                height={shot.height}
                loading="lazy"
                decoding="async"
              />
              <span className="shot-btn__bar">
                <span className="shot-btn__title">{shot.title}</span>
                <span className="shot-btn__icon" aria-hidden="true">
                  <ExpandIcon />
                </span>
              </span>
              <span className="visually-hidden"> — open a larger view</span>
            </button>
          </li>
        ))}
      </ul>

      <dialog
        ref={dialogRef}
        className="lightbox"
        aria-label={active ? `${active.title}, larger view` : 'Screenshot'}
        onClick={onDialogClick}
      >
        {active && (
          <div className="lightbox__inner">
            <div className="lightbox__bar">
              <p className="lightbox__title">{active.title}</p>
              <button type="button" className="icon-btn" onClick={close} aria-label="Close">
                <CloseIcon />
              </button>
            </div>
            <img
              className="lightbox__image"
              src={asset(active.src)}
              alt={active.caption}
              width={active.width}
              height={active.height}
            />
            <p className="lightbox__caption">{active.caption}</p>
          </div>
        )}
      </dialog>
    </div>
  );
}
