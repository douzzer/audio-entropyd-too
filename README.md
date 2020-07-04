# audio-entropyd-too

This is a fork of Folkert van Heusden's `audio-entropyd` daemon, based
on [`audio-entropyd-2.0.3.tgz`](https://www.vanheusden.com/aed/audio-entropyd-2.0.3.tgz).

This is a work in progress, but I've been running it for some time, fed
by a Geiger-Müller sensor stimulated passively by environmental radiation,
with good results.  Best sample rate for this purpose is 192k.

## Usage

```
Usage: audio-entropyd-too [options]

Collect entropy from a soundcard and feed it into the kernel random pool.

Options:
--device,       -d []  Specify sound device to use. (Default hw:0)
--sample-rate,  -N []  Audio sampling rate. (default 11025)
--spike-mode,   -k     Continually search for spikes (typically from a Geiger counter) and seed from inter-spike interval
--spike-threshold-percent, -t []  Threshold for spike detection, negative for negative-going spikes
--spike-edge-min-delta-percent, -T []  Minimum change in consecutive sample value for an above-threshold sample to qualify as a spike onset
--spike-channel-mask, -c []  Mask of channels to search for spikes in, bitwise-or of 1 for channel zero, 2 for channel one
--spike-minimum-interval-frames, -i []  Reject spikes closer than this many raw frames apart (relative to requested sample rate)
--spike-test-mode      Run spike mode for testing -- print events, and don't add entropy to the entropy pool
--spike-log <path>     Record spike histogram data to <path>
--spike-log-interval-seconds []   Duration of histogram bins in seconds
--skip-test,    -s     Do not check if data is random enough.
--do-not-fork   -n     Do not fork.
--file <path>   -f     Store raw randomness data to path (while still adding randomness to kernel pool).
--verbose,      -v     Be verbose.
--help,         -h     This help.
```

Note that in `--spike-mode`, randomness stored with `--file` is
completely raw, to expose any statistical regularities, whereas it is
whitened with AES128, using an unrecorded one-time key, before passing
it to the kernel randomness pool.

### Example invocation

For Geiger-Müller input on left channel of a 192k soundcard at `hw:0`
(the default device):
```
/usr/local/sbin/audio-entropyd-too -N 192000 --file /var/log/audio-entropyd.random --spike-mode --spike-threshold-percent 50 --spike-edge-min-delta-percent 20 --spike-channel-mask 1 --spike-minimum-interval-frames 100 --spike-log /var/log/radiation.log --spike-log-interval-seconds 60```
```

## Performance

Internal statistical characterization (`--spike-log`) produces results like this:

```
last log age: 59 s
interval: 60.076 s
interval count: 27 (Z -0.3)
interval burstiness=2.7
interval entropy: 526 bits
cum bit balance: 50.005197% (Z +0.8)
cum byte avg: 127.492 (Z -0.3)
ChiSquare score: 263.62 (Z +0.4)
n_bytes=6992560
n_zero_bytes=27166 (Z -0.90)
n_ones_bytes=27456 (Z +0.86)
```
From raw log lines like this:
```
2020-07-04T20:27:58.491477Z N C0=27 C/sd=-0.3 E=526 B=46.388% Bcum=50.005197% Bcum/sd=+0.8 A=116.1 Acum=127.492 Acum/sd=-0.3 ChiSq=263.62 ChiSq/sd=+0.4 n=6992560 z=27166 o=27456 m_hz=1.68 brst=2.73
```

(My "burstiness" metric is poorly constructed and will likely be
replaced in the future.)
