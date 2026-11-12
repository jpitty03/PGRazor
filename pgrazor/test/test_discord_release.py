import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import discord_release


def release(assets, **extra):
    base = {'tag_name': 'v0.1.6', 'name': 'PGRazor v0.1.6',
            'html_url': 'https://github.com/example/pgrazor/releases/tag/v0.1.6',
            'body': 'Quick menu improvements.', 'assets': assets}
    base.update(extra)
    return base


APK = {'name': 'PGRazor.apk',
       'browser_download_url': 'https://example.com/PGRazor.apk'}
HIGHLIGHT = {'name': 'release-highlight.png',
             'browser_download_url': 'https://example.com/release-highlight.png'}


class DiscordReleaseTests(unittest.TestCase):
    def test_both_root_downloads_are_linked_alongside_normal_apk(self):
        roots = [{'name': name, 'browser_download_url': 'https://example.com/' + name}
                 for name in ('PGRazor-Root.apk', 'PGRazor-Root-module.zip')]
        fields = discord_release.message(release([APK, *roots]))['embeds'][0]['fields']
        for asset in [APK, *roots]:
            self.assertTrue(any(asset['browser_download_url'] in field['value'] for field in fields))

    def test_highlight_image_present_sets_embed_image(self):
        payload = discord_release.message(release([APK, HIGHLIGHT]))
        embed = payload['embeds'][0]
        self.assertEqual(embed['image']['url'], HIGHLIGHT['browser_download_url'])
        # APK download link is preserved alongside the screenshot.
        self.assertTrue(any(f['value'] == '[Install PGRazor.apk](' + APK['browser_download_url'] + ')'
                            for f in embed['fields']))

    def test_no_highlight_asset_omits_embed_image(self):
        payload = discord_release.message(release([APK]))
        embed = payload['embeds'][0]
        self.assertNotIn('image', embed)
        self.assertTrue(any(f['name'] == 'Download' for f in embed['fields']))

    def test_role_ping_preserved_with_image(self):
        role = '123456789012345678'
        # Without a role id there is no ping content at all.
        self.assertNotIn('content', discord_release.message(release([APK, HIGHLIGHT])))
        payload = discord_release.message(release([APK, HIGHLIGHT]), role)
        self.assertEqual(payload['content'], '<@&' + role + '> A new PGRazor release is available!')
        self.assertEqual(payload['allowed_mentions']['roles'], [role])
        self.assertEqual(payload['allowed_mentions']['parse'], [])
        self.assertEqual(payload['embeds'][0]['image']['url'], HIGHLIGHT['browser_download_url'])


if __name__ == '__main__':
    unittest.main()
