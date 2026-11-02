"""Fetch the private, pinned game input and decode the CI signing key without logging secrets."""
import base64
import hashlib
import os
from pathlib import Path
import urllib.parse
import urllib.request
ROOT = Path(__file__).resolve().parents[1]/'pgrazor'
class HttpsRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != 'https': raise ValueError('Insecure redirect')
        redirected=super().redirect_request(req,fp,code,msg,headers,newurl)
        if redirected and urllib.parse.urlsplit(req.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            redirected.remove_header('Authorization')
        return redirected

def main():
    url=os.environ['PGRAZOR_INPUT_URL']
    if urllib.parse.urlsplit(url).scheme!='https': raise SystemExit('Input URL must use HTTPS')
    folder=ROOT/'input';folder.mkdir(exist_ok=True)
    output=folder/'game.apkm'
    headers={}
    if urllib.parse.urlsplit(url).hostname=='api.github.com':
        headers={'Accept':'application/octet-stream','Authorization':'Bearer '+os.environ['GITHUB_TOKEN']}
    opener=urllib.request.build_opener(HttpsRedirect())
    try:
        with opener.open(urllib.request.Request(url,headers=headers),timeout=90) as source,output.open('wb') as dest:
            if urllib.parse.urlsplit(source.url).scheme!='https': raise ValueError('Insecure redirect')
            total=0
            while chunk:=source.read(1024*1024):
                total+=len(chunk)
                if total>2_000_000_000: raise ValueError('Input too large')
                dest.write(chunk)
    except Exception: raise SystemExit('Private game input download failed; check the secret URL') from None
    expected='7642787f991a30a6249b9225ee1d224a0e6465ba0c85f9f0e2ff781a97f089f3'
    with output.open('rb') as stream:
        if hashlib.file_digest(stream,'sha256').hexdigest()!=expected: raise SystemExit('Unsupported game input checksum')
    key=Path(os.environ['PGRAZOR_KEYSTORE']);key.parent.mkdir(parents=True,exist_ok=True)
    key.write_bytes(base64.b64decode(os.environ['PGRAZOR_KEYSTORE_BASE64'],validate=True))
if __name__=='__main__': main()
