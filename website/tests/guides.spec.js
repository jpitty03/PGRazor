import { test, expect } from '@playwright/test';
import { featureGuides } from '../src/featureGuides.js';

test('feature guides have unique destinations and useful instructions', async ({ page }) => {
  const ids = featureGuides.map(guide => guide.id);
  expect(new Set(ids).size).toBe(ids.length);
  expect(ids).toEqual(expect.arrayContaining([
    'gym-slotter', 'batch-evolution', 'buddy-emotion', 'vgp-plus',
    'world-map', 'event-boxes', 'inventory-cleanup',
  ]));
  await page.goto('./?guide=all');
  await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
  for (const guide of featureGuides) {
    expect(guide.steps.length).toBeGreaterThan(1);
    expect(guide.where.length).toBeGreaterThan(0);
    expect(guide.availability.length).toBeGreaterThan(0);
    await expect(page.locator(`main a[href$="?guide=${guide.id}"]`).first()).toBeVisible();
  }
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
});

test('every guide can be opened directly without missing content or script errors', async ({ page }) => {
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  for (const guide of featureGuides) {
    await page.goto(`./?guide=${guide.id}`);
    await expect(page.getByRole('heading', { level: 1, name: guide.title, exact: true })).toBeVisible();
    expect(await page.title()).toContain(guide.title);
    await expect(page.locator('main').getByText(guide.availability, { exact: true }).first()).toBeVisible();
    for (const step of guide.steps) {
      await expect(page.locator('main').getByText(step, { exact: true })).toHaveCount(1);
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
  }
  expect(errors).toEqual([]);
});

test('tag guide supports reload, back navigation and returning home', async ({ page }) => {
  await page.goto('./?guide=all');
  await page.locator('main a[href$="?guide=batch-evolution"]').first().click();
  await expect(page).toHaveURL(/\?guide=batch-evolution$/);
  await page.reload();
  await expect(page.getByRole('heading', { level: 1, name: featureGuides.find(g => g.id === 'batch-evolution').title })).toBeVisible();
  await page.goBack();
  await expect(page).toHaveURL(/\?guide=all$/);
  await page.goto('./?guide=gym-slotter');
  const toggle = page.getByRole('button', { name: 'Open menu' });
  if (await toggle.isVisible()) await toggle.click();
  await page.getByRole('navigation', { name: 'Primary' }).getByRole('link', { name: 'Features', exact: true }).click();
  await expect(page.locator('#features')).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Set up your kind of session.' })).toHaveCount(0);
});

test('unknown guide offers a route back to the guide index', async ({ page }) => {
  await page.goto('./?guide=not-a-real-feature');
  await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
  await page.locator('main a[href$="?guide=all"]').first().click();
  await expect(page).toHaveURL(/\?guide=all$/);
});

test('guide search narrows results and recovers from no matches', async ({ page }) => {
  await page.goto('./?guide=all');
  const search = page.getByRole('searchbox', { name: 'Search guides' });
  await search.fill('Batch evolution');
  await expect(page.locator('.guide-card')).toHaveCount(1);
  await expect(page.locator('.guide-card')).toHaveAttribute('href', /\?guide=batch-evolution$/);
  await search.fill('unmatched-guide-search');
  await expect(page.locator('.guide-card')).toHaveCount(0);
  await page.getByRole('button', { name: 'Clear search' }).click();
  await expect(page.locator('.guide-card')).toHaveCount(featureGuides.length);
});
