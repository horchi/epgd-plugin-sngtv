# epgd-plugin-sngtv

EPG loader plugin for [epgd](https://github.com/horchi/vdr-epg-daemon) using the
public JSON interface of the *sngtv* EPG backend (the backend of a German IPTV
web player). It is accessed anonymously as a guest, no account is needed.

**This plugin is not affiliated with, endorsed or supported by the operator of
that service.** The interface is undocumented and may change or vanish at any time.

## Legal notes - please read

- The programme data are protected by database rights (§§ 87a ff. UrhG).
  Use this plugin for **private, non-commercial purposes only**.
- **Pictures are disabled by default.** They are licensed third-party content
  (press pictures of broadcasters and picture agencies). Only enable them
  (`sngtv.images = 1`) for your own private use.
- Do not make an epgd / epghttpd instance fed by this plugin reachable from
  the internet and do not redistribute the data or pictures.
- Keep the default request delay - be gentle to the server.
- Using this plugin is at your own risk.

## Data

Per event: title, episode title, description, genre, season/episode number,
country, year, actors, director, producer and optionally one picture. The
service offers the pictures only in their original size (up to 1920x1080),
the plugin scales them down (default 960x540) before epgd stores them. The
data reach about 14 days ahead.

Compared to tvm (TV Movie): the descriptions are shorter and there are no
ratings/tips, but the pictures have a much higher resolution and there are
season/episode numbers. A good setup is tvm as leading provider (merge 1)
and sngtv as fall-back whose pictures replace the tvm ones (merge 3).

## Build / Install

Clone into the `PLUGINS` directory of your epgd source and build:

```
cd epgd/PLUGINS
git clone <this repo> epgd-plugin-sngtv
cd epgd-plugin-sngtv
make && make install
```

`make install` installs

- the plugin to the epgd plugin directory,
- `configs/sngtv-*.xsl` and `configs/sngtv-category.xml` to `/etc/epgd`
  (existing files are not overwritten),
- the helper script `scripts/sngtv-channels.py` as `sngtv-channels` to
  `$(PREFIX)/bin` (default `/usr/local/bin`), see *Channel map*.

Append `configs/epgd.conf` to `/etc/epgd/epgd.conf` and adjust it.

## Channel map

The channel ids of the service are not the DVB ids, so the lines for your
`channelmap.conf` have to be built once. The helper script `sngtv-channels`
(Python 3, no further modules needed) does this for you.

### List all channels

```
sngtv-channels
```

prints the id and name of every channel the service offers, e.g.

```
   373  Das Erste
   408  ZDF
   404  RTL
```

### Propose channelmap lines

```
sngtv-channels -c /var/lib/vdr/channels.conf [-e /etc/epgd/channelmap.conf] > sngtv-map.txt
```

| option                   | default  | description                                                          |
|--------------------------|----------|----------------------------------------------------------------------|
| `-c`, `--channels-conf`  |          | VDR `channels.conf` to match against                                 |
| `-s`, `--source`         | `S19.2E` | only VDR channels of this source (empty: all sources)                |
| `-m`, `--min-ratio`      | `0.6`    | minimal name similarity (0..1) if the service id confirms the match  |
| `-n`, `--min-name-ratio` | `0.85`   | minimal name similarity (0..1) for a match by the name only          |
| `-e`, `--existing`       |          | your `channelmap.conf`, marks proposals already configured there     |
| `--radio`                |          | include the radio channels of the `channels.conf`                    |
| `--unmatched`            |          | list the sngtv channels without a matching VDR channel at the end    |

For each channel of the service the script fetches a few events to get the DVB
service id, then it picks the best matching sngtv channel for each VDR channel
(an exact name match wins, `HD`, `SD`, `TV` and `Fernsehen` are ignored, so the
HD and the SD channel of a station get the same proposal). The service id the
API reports is the one of the MagentaTV IPTV network, it equals the satellite
one only for some channels (e.g. ARD, ZDF), so it is used as a confirmation
only. It runs about 2-5 minutes. The output follows the order and the groups of
the `channels.conf`:

- `sngtv:<id> = <channelid> // <name> -> <vdr name>` - a proposed line, with notes:
  `name only` (the service id does not confirm the match), the similarity if it
  is no exact match, `configured` or `configured as ...!` (with `-e`),
- `// <channelid> // <name> - no match` - VDR channels without a proposal.

**Review the proposal before using it!** Especially `name only` lines with a
similarity below 1 may be wrong (e.g. regional variants or similar names). Copy
only the lines of the channels you want into `/etc/epgd/channelmap.conf` and add
the merge value.

### Example

Texts from tvm, sngtv as fall-back and with its (larger) pictures:

```
tvm:1       = S19.2E-1-1019-10301   // Das Erste HD  (merge 1, leading)
sngtv:373:3 = S19.2E-1-1019-10301   // Das Erste     (merge 3, fall-back + pictures)
```

Use merge `2` instead of `3` to keep the pictures of the leading provider, or
merge `1` if sngtv is your only provider for this channel. See *EPG Merge* in
the epgd README for the meaning of the merge values.

## Options (epgd.conf)

| option               | default | description                                              |
|----------------------|---------|----------------------------------------------------------|
| `sngtv.images`       | 0       | download pictures (see legal notes!), needs `GetEPGImages = 1` too |
| `sngtv.imageType`    | 17      | preferred picture: 1 = 4:3, 17 = 16:9, 18 = background   |
| `sngtv.imageMaxWidth`  | 960   | scale the pictures down to fit into this box, 0 = no limit |
| `sngtv.imageMaxHeight` | 540   |                                                          |
| `sngtv.imageQuality` | 80      | jpeg quality of the scaled pictures (10..100)            |
| `sngtv.requestDelay` | 250     | pause between requests [ms]                              |
| `sngtv.timeout`      | 30      | request timeout [s]                                      |
| `sngtv.saveJson`     | 0       | debug: store raw json in `<CachePath>/sngtv`            |
| `sngtv.saveXml`      | 0       | debug: store intermediate xml in `<CachePath>/sngtv`    |

## Notes

- Event ids are the provider ids plus 3000000000 to keep them apart from
  other sources.
- Category mapping: `configs/sngtv-category.xml`, transformation:
  `configs/sngtv-utf-8.xsl` (and `-iso-8859-1`).

## License

GPL-2.0-or-later, like epgd - see [LICENSE](LICENSE).
