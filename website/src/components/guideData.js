import { asset } from '../assets.js';
export { featureGuides } from '../featureGuides.js';

// Query routes work on the static GitHub Pages deployment, including reloads.
export const homeHref = asset('');
export const indexHref = `${homeHref}?guide=all`;
export function guideHref(id) {
  return `${homeHref}?guide=${encodeURIComponent(id)}`;
}
