import { test, expect } from '@playwright/test';

test('production page loads its assets and stays within the viewport', async ({ page }) => {
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto('./');
  await expect(page.locator('h1')).toBeVisible();
  await expect(page.locator('a[href="https://github.com/jpitty03/PGRazor/releases/latest"]').first()).toBeVisible();
  for (const image of await page.locator('img').all()) {
    await image.scrollIntoViewIfNeeded();
    await expect.poll(() => image.evaluate(img => img.complete && img.naturalWidth > 0)).toBeTruthy();
  }
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
  expect(errors).toEqual([]);
});

test('in-page navigation points to existing content', async ({ page }) => {
  await page.goto('./');
  const missing = await page.locator('a[href^="#"]').evaluateAll(links =>
    links.map(link => link.getAttribute('href')).filter(href => href.length < 2 || !document.getElementById(href.slice(1)))
  );
  expect(missing).toEqual([]);
});

test('defaults to dark and remembers the chosen theme', async ({ page }) => {
  await page.emulateMedia({ colorScheme: 'light' });
  await page.goto('./');
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.getByRole('button', { name: 'Switch to light theme' }).click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await page.reload();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await page.getByRole('button', { name: 'Switch to dark theme' }).click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
});

test('settings guide and screenshot viewer work with the keyboard', async ({ page }) => {
  await page.goto('./');
  const catching = page.getByRole('tab', { name: 'Catching', exact: true });
  await catching.focus();
  await page.keyboard.press('ArrowRight');
  await expect(page.getByRole('tab', { name: 'Explore', exact: true })).toHaveAttribute('aria-selected', 'true');
  await expect(page.getByRole('tabpanel', { name: 'Explore', exact: true })).toBeVisible();
  const shot = page.locator('.shot-btn').first();
  await shot.focus();
  await page.keyboard.press('Enter');
  await expect(page.getByRole('dialog')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).not.toBeVisible();
  await expect(shot).toBeFocused();
});

test('cooldown nav link reveals the section and its estimate table', async ({ page }) => {
  await page.goto('./');
  // The nav link lives behind the hamburger on narrow viewports; open it first.
  const toggle = page.getByRole('button', { name: 'Open menu' });
  if (await toggle.isVisible()) {
    await toggle.click();
  }
  await page.locator('a[href="#cooldown"]').first().click();
  const section = page.locator('#cooldown');
  await expect(section).toBeVisible();
  await expect(section.getByRole('heading', { level: 2 })).toBeVisible();

  const table = section.locator('table.cooldown__table');
  await expect(table.getByRole('columnheader', { name: 'Distance (km)' })).toBeVisible();
  await expect(table.getByRole('columnheader', { name: /Reported wait/ })).toBeVisible();
  // Endpoints of the chart confirm the data rendered in order.
  await expect(table.getByRole('rowheader', { name: '1', exact: true })).toBeVisible();
  await expect(table.getByRole('rowheader', { name: '1350+' })).toBeVisible();
  await expect(table.getByRole('cell', { name: '120 min' })).toBeVisible();
});

test('cooldown trigger and safe groups expand', async ({ page }) => {
  await page.goto('./');
  const safe = page.locator('.cooldown__group', { hasText: 'Does not start it' });
  await expect(safe.getByText('Teleporting by itself')).toBeHidden();
  await safe.locator('summary').click();
  await expect(safe.getByText('Teleporting by itself')).toBeVisible();
});

test('experimental prerelease link is present and distinct from the stable download', async ({ page }) => {
  await page.goto('./');
  const experimental = page.locator(
    'a[href="https://github.com/jpitty03/PGRazor/releases/tag/v0.1.7-experimental.1"]'
  );
  await expect(experimental).toHaveCount(1);
  await expect(experimental).toBeVisible();
  // It must not masquerade as the primary "latest release" call to action.
  await expect(experimental).not.toHaveClass(/btn--primary/);
  await expect(page.locator('.cta__badge')).toHaveText('Experimental');
});
