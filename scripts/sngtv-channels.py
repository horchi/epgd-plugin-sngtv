#!/usr/bin/env python3
#
# sngtv-channels.py
#
# Helper for the epgd sngtv plugin: lists the channels offered by the
# sngtv EPG API and (optionally) proposes channelmap.conf lines by matching
# them against a VDR channels.conf.
#
# Matching is done per VDR channel by a fuzzy name comparison, the best
# sngtv channel wins. The DVB service id the API reports for the events
# (dvbEventTriplet) is the one of the MagentaTV IPTV network, it equals the
# satellite one only for some channels (e.g. ARD, ZDF) and is used as a
# confirmation only. Always review the result before using it!
#
# installed as 'sngtv-channels' by 'make install'
#
# usage:
#   sngtv-channels                             # list channels (id / name)
#   sngtv-channels -c /var/lib/vdr/channels.conf [-s S19.2E] [-m 0.6] [-n 0.85]
#                  [-e /etc/epgd/channelmap.conf] [--radio] [--unmatched]
#                                              # propose channelmap lines
#

import argparse
import difflib
import http.cookiejar
import json
import re
import socket
import sys
import time
import urllib.request

API = 'https://api.prod.sngtv.magentatv.de/EPG/JSON'

# resolve each host only once, urllib looks it up again for every request
#   (a slow resolver would slow down each of the some hundred requests)

_getaddrinfo = socket.getaddrinfo
_addrCache = {}

def _cachedGetaddrinfo(*args, **kwargs):
   key = (args, tuple(sorted(kwargs.items())))
   if key not in _addrCache:
      _addrCache[key] = _getaddrinfo(*args, **kwargs)
   return _addrCache[key]

socket.getaddrinfo = _cachedGetaddrinfo

class Api:
   def __init__(self):
      self.jar = http.cookiejar.CookieJar()
      self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))
      self.token = None

   def post(self, path, body, query=''):
      req = urllib.request.Request(API + path + query, data=json.dumps(body).encode(), method='POST')
      req.add_header('Content-Type', 'application/json')
      if self.token:
         req.add_header('X_CSRFTOKEN', self.token)
      with self.opener.open(req, timeout=10) as r:
         return json.load(r)

   def login(self):
      res = self.post('/Authenticate', {
         'terminalid': '00:00:00:00:00:00', 'mac': '00:00:00:00:00:00',
         'terminaltype': 'WEBTV', 'utcEnable': 1, 'timezone': 'Etc/GMT0',
         'userType': 3, 'terminalvendor': 'Unknown' }, '?SID=firstup&T=Windows_chrome_118')
      self.token = res.get('csrfToken')
      if not self.token:
         sys.exit('authentication failed: %s' % res.get('retmsg'))

   def channels(self):
      res = self.post('/AllChannel', {
         'channelNamespace': 2, 'filterlist': [{'key': 'IsHide', 'value': '-1'}],
         'metaDataVer': 'Channel/1.1',
         'properties': [{'include': '/channellist/logicalChannel/contentId,/channellist/logicalChannel/name',
                         'name': 'logicalChannel'}],
         'returnSatChannel': 0 })
      return [(c['contentId'], c['name']) for c in res.get('channellist', [])]

   def serviceId(self, cid):
      day = time.strftime('%Y%m%d')
      res = self.post('/PlayBillList', {
         'count': 5, 'isFillProgram': 0, 'offset': 0, 'type': 2,
         'properties': [{'include': 'id,externalIds', 'name': 'playbill'}],
         'begintime': day + '000000', 'endtime': day + '235959', 'channelid': cid })
      for e in res.get('playbilllist', []):
         try:
            for x in json.loads(e.get('externalIds') or '[]'):
               if x.get('type') == 'dvbEventTriplet':
                  return int(x['id'].split(':')[0])
         except (ValueError, KeyError):
            pass
      return None

# abbreviations used in channels.conf names

ALIASES = { 'pro7': 'prosieben', 'fs': '', 'hh': 'hamburg', 'mv': 'mecklenburgvorpommern',
            'sh': 'schleswigholstein', 'nds': 'niedersachsen', 'rlp': 'rp' }

def norm(name):
   name = name.lower().replace('&', 'und').replace('+', 'plus')
   name = ' '.join(ALIASES.get(w, w) for w in re.split(r'[\s_-]+', name))
   name = re.sub(r'\b(hd|uhd|sd|fernsehen|television|tv)\b', '', name)
   return re.sub(r'[^a-z0-9äöü]', '', name)

def readVdrChannels(path, source, radio):
   chans = []   # dict(id, name, sid, group)
   group = ''
   with open(path, encoding='utf-8', errors='replace') as f:
      for line in f:
         line = line.rstrip('\n')
         if line.startswith(':'):
            group = line[1:].lstrip('->=*~ ')
            continue
         p = line.split(':')
         if len(p) < 13 or (source and p[3] != source):
            continue
         if not radio and p[5] in ('0', ''):          # no video pid -> radio
            continue
         name = p[0].split(';')[0].split(',')[0]
         chans.append({ 'id': '%s-%s-%s-%s' % (p[3], p[10], p[11], p[9]), 'name': name, 'sid': int(p[9]), 'group': group })
   return chans

def readChannelmap(path):
   rows = {}    # channelid -> [ 'sngtv:<id>[:merge]', ... ]
   with open(path, encoding='utf-8', errors='replace') as f:
      for line in f:
         m = re.match(r'^\s*(sngtv:[^=\s]+)\s*=\s*([^\s/]+)', line)
         if m:
            for cid in m.group(2).split(','):
               rows.setdefault(cid.strip(), []).append(m.group(1))
   return rows

def ratio(a, b):
   return difflib.SequenceMatcher(None, norm(a), norm(b)).ratio()

def main():
   ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
   ap.add_argument('-c', '--channels-conf', help='VDR channels.conf to match against')
   ap.add_argument('-s', '--source', default='S19.2E', help='VDR source to match (default S19.2E, empty for all)')
   ap.add_argument('-m', '--min-ratio', type=float, default=0.6, help='minimal name similarity if the service id matches too (default 0.6)')
   ap.add_argument('-n', '--min-name-ratio', type=float, default=0.85, help='minimal name similarity for a match by name only (default 0.85)')
   ap.add_argument('-e', '--existing', help='channelmap.conf, mark the proposals already configured there')
   ap.add_argument('--radio', action='store_true', help='include radio channels of the channels.conf')
   ap.add_argument('--unmatched', action='store_true', help='list the sngtv channels without a matching VDR channel')
   a = ap.parse_args()

   api = Api()
   api.login()
   chans = api.channels()

   if not a.channels_conf:
      for cid, name in sorted(chans, key=lambda c: c[1].lower()):
         print('%6s  %s' % (cid, name))
      return

   vdr = readVdrChannels(a.channels_conf, a.source, a.radio)
   existing = readChannelmap(a.existing) if a.existing else {}

   # service id of each sngtv channel (one request per channel)

   sngtv = []
   for n, (cid, name) in enumerate(sorted(chans, key=lambda c: c[1].lower())):
      try:
         sid = api.serviceId(cid)
      except Exception as e:
         print('warning: %s (%s): %s' % (cid, name, e), file=sys.stderr)
         sid = None
      sngtv.append({ 'id': cid, 'name': name, 'sid': sid })
      print('\r%d/%d' % (n + 1, len(chans)), end='', file=sys.stderr, flush=True)
      time.sleep(0.2)   # be gentle
   print(file=sys.stderr)

   # best sngtv channel per VDR channel: matching service id first, then the name similarity

   used = set()
   group = None
   print('// proposals for %s, review before use!' % a.channels_conf)
   print('//   (name only) - the service id does not confirm the match')

   for v in vdr:
      best, bestKey = None, None
      for s in sngtv:
         r = ratio(s['name'], v['name'])
         sidMatch = s['sid'] is not None and s['sid'] == v['sid']
         if r < (a.min_ratio if sidMatch else a.min_name_ratio):
            continue
         key = (r >= 0.999, sidMatch, r, -abs(len(s['name']) - len(v['name'])))
         if bestKey is None or key > bestKey:
            best, bestKey = s, key

      if v['group'] != group:
         group = v['group']
         print('//\n// --- %s' % (group or '-'))

      if not best:
         print('// %-14s   %s // %s - no match' % ('', v['id'], v['name']))
         continue

      used.add(best['id'])
      line = '%-14s = %s' % ('sngtv:' + best['id'], v['id'])
      notes = []
      if not bestKey[1]:
         notes.append('name only')
      if bestKey[2] < 0.999:
         notes.append('%.2f' % bestKey[2])
      have = existing.get(v['id'], [])
      if any(h.split(':')[1] == best['id'] for h in have):
         notes.append('configured')
      elif have:
         notes.append('configured as ' + ', '.join(have) + '!')
      print('%s\t// %s -> %s%s' % (line, best['name'], v['name'], '  (' + ', '.join(notes) + ')' if notes else ''))

   if a.unmatched:
      print('//\n// --- sngtv channels without a matching VDR channel')
      for s in sngtv:
         if s['id'] not in used:
            print('// sngtv:%-6s %s (sid %s)' % (s['id'], s['name'], s['sid']))

if __name__ == '__main__':
   main()
