"""Announce a published release using a channel-bound Discord webhook."""
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def request(url, *, token=None, payload=None):
    headers = {'User-Agent': 'PGRazor-release-announcer', 'Accept': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    data = None
    if payload is not None:
        headers['Content-Type'] = 'application/json'
        data = json.dumps(payload).encode('utf-8')
    req = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.build_opener(NoRedirect()).open(req, timeout=30) as response:
        return json.load(response)


def message(release, role_id=''):
    role_id = role_id.strip()
    if role_id and not re.fullmatch(r'[0-9]{17,20}', role_id):
        raise ValueError('DISCORD_RELEASE_ROLE_ID must be a numeric Discord role ID')
    kind = 'Prerelease' if release.get('prerelease') else 'Release'
    notes = (release.get('body') or 'See GitHub for release details.').strip()
    if len(notes) > 3000:
        notes = notes[:2997] + '...'
    assets = release.get('assets', [])
    apk = next((a['browser_download_url'] for a in assets if a.get('name') == 'PGRazor.apk'), None)
    highlight = next((a['browser_download_url'] for a in assets if a.get('name') == 'release-highlight.png'), None)
    fields = [{'name': 'Download', 'value': '[Install PGRazor.apk](' + apk + ')'}] if apk else []
    for filename, label in (('PGRazor-Root.apk', 'Root installer APK'),
                            ('PGRazor-Root-module.zip', 'Root module ZIP')):
        asset = next((a for a in assets if a.get('name') == filename), None)
        if asset:
            fields.append({'name': label, 'value': '[' + filename + '](' + asset['browser_download_url'] + ')'})
    # Keep the support link visible even when long release notes are truncated.
    if 'https://ko-fi.com/pgrazor' not in notes:
        fields.append({'name': 'Support development', 'value': '[Ko-fi](https://ko-fi.com/pgrazor) (optional)'})
    embed = {'title': (release.get('name') or release['tag_name'])[:256],
             'url': release['html_url'], 'description': notes,
             'color': 0x35DAB4, 'fields': fields,
             'footer': {'text': 'PGRazor | ' + kind}}
    # Optional release highlight screenshot shown inline in the embed when present.
    if highlight:
        embed['image'] = {'url': highlight}
    payload = {'username': 'PGRazor Releases', 'allowed_mentions': {'parse': []},
            'embeds': [embed]}
    if role_id:
        payload['content'] = '<@&' + role_id + '> A new PGRazor release is available!'
        payload['allowed_mentions']['roles'] = [role_id]
    return payload


def main():
    webhook = os.environ.get('DISCORD_RELEASE_WEBHOOK', '').strip()
    if not webhook:
        print('::warning::Add the DISCORD_RELEASE_WEBHOOK Actions secret to enable announcements.')
        return
    if not re.fullmatch(r'https://discord\.com/api(?:/v\d+)?/webhooks/\d+/[A-Za-z0-9_.-]+', webhook):
        raise ValueError('DISCORD_RELEASE_WEBHOOK must be a Discord webhook URL')
    repo = os.environ['GITHUB_REPOSITORY']
    tag = urllib.parse.quote(os.environ['RELEASE_TAG'], safe='')
    release = request('https://api.github.com/repos/' + repo + '/releases/tags/' + tag,
                      token=os.environ['GITHUB_TOKEN'])
    if release.get('draft') or not release.get('published_at'):
        raise ValueError('Only published releases can be announced')
    payload = message(release, os.environ.get('DISCORD_RELEASE_ROLE_ID', ''))
    hook = request(webhook)
    if str(hook.get('channel_id')) != os.environ['DISCORD_CHANNEL_ID']:
        raise ValueError('Webhook belongs to a different channel; nothing sent')
    # No automatic POST retry: a lost response must not blindly duplicate announcements.
    result = request(webhook + '?wait=true', payload=payload)
    if not result.get('id'):
        raise ValueError('Discord did not confirm the announcement')
    print('Release announcement sent.')


if __name__ == '__main__':
    try:
        main()
    except urllib.error.HTTPError as error:
        print('::error::Release announcement failed with HTTP ' + str(error.code))
        sys.exit(1)
    except Exception as error:
        # Network exceptions may contain the secret webhook URL. Never print their text.
        print('::error::Release announcement failed (' + type(error).__name__ + '). Check configuration and retry manually.')
        sys.exit(1)
