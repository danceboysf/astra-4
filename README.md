# Description

Astra (Advanced Streamer) is a professional software to organize Digital TV Service for
TV operators and broadcasters, internet service providers, hotels, etc.

* Open source version not maintained. Please, check https://cesbo.com/astra/ to get more info.
* Telegram Community: [EN](https://t.me/cesbo_en) [RU](https://t.me/cesbo_ru)

## Installation (Debian/Ubuntu)

```
sudo apt update
sudo apt install build-essential pkg-config libdvbcsa-dev

./configure.sh --with-libdvbcsa --bin=/usr/bin/astra
make -j"$(nproc)"
sudo make install
```

### Burst-on-connect sizing examples

`--burst-size` expects kilobytes. The buffer must always be at least one
transport-stream packet (188 bytes) larger than the burst. For a 5 MB
prefill (5120 KB), the following relay invocations stay valid:

* `astra --relay --burst-size 5120 --buffer-size 5320 ...`
  (200 KB headroom keeps well above the minimum extra 188 bytes.)
* `astra --relay --burst-size 5120 --buffer-size 5200 ...`
  (80 KB of margin still satisfies the one-packet rule.)

Any other values work as long as `buffer_size > burst_size` and the gap is at
least 188 bytes.
