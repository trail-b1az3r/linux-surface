# Location ("GPS") on Surface devices

## Does my Surface Pro 5 have GPS?

Only the **LTE model** does. Check yours with
`cat /sys/class/dmi/id/product_sku`:

| SKU | Model | Location hardware |
|---|---|---|
| `Surface_Pro_1796` | Surface Pro 5, Wi-Fi | **No GPS receiver.** Location comes from nearby Wi-Fi networks, just like on Windows |
| `Surface_Pro_1807` | Surface Pro 5, LTE Advanced | GPS/GLONASS (standalone and assisted) in the Qualcomm Snapdragon X16 modem |

On the Wi-Fi model, no driver can add satellite GPS. The hardware isn't
there. For real GPS on it, plug in a USB GPS receiver; see the end of this
page.

## What this sets up

Linux apps get their location from **GeoClue**: GNOME Maps, Firefox, weather
apps, night light, and the automatic time zone. `surface-location-setup.sh`
configures it for Surface devices:

- **Wi-Fi positioning (all models).** GeoClue looks up the Wi-Fi networks
  around you in [beaconDB](https://beacondb.net/). beaconDB is an open,
  opt-in replacement for the retired Mozilla Location Service. Wi-Fi must be
  on, but it doesn't need to be connected. Accuracy is typically tens of
  metres in towns, worse in the countryside.
- **GPS (LTE model).** GeoClue reads GPS through ModemManager. The script
  installs and starts ModemManager, and enables GeoClue's `modem-gps` source.
- **Cell-tower positioning (LTE model with a SIM)** uses the same lookup
  service.

```sh
./surface-location-setup.sh              # what does this device support, what's missing
sudo ./surface-location-setup.sh --fix   # install GeoClue (+ ModemManager), install config
./surface-location-setup.sh --test       # print the current location
```

The config is `90-surface-location.conf`, installed to
`/etc/geoclue/conf.d/`.

## Important: Wi-Fi must use wpa_supplicant

GeoClue reads Wi-Fi scan results from **wpa_supplicant only**. If your
NetworkManager uses **iwd** as its Wi-Fi backend, Wi-Fi positioning silently
returns nothing. The check script detects this and prints the commands to
switch. Your saved networks stay.

## Privacy

To find you, GeoClue sends the nearby Wi-Fi access point IDs to beaconDB.
That's the same kind of data Windows sends to Microsoft. Apps still need your
permission: your desktop asks the first time, and you can revoke it in its
privacy settings. To use no online lookup at all, set `enable=false` under
`[wifi]` and `[3g]`.

## External USB GPS (for the Wi-Fi model)

Any USB GPS that outputs NMEA (most do, e.g. u-blox based) works with
`gpsd`. Apps that speak gpsd (navit, foxtrotgps, QGIS) can then use it
directly:

```sh
sudo pacman -S gpsd
sudo systemctl enable --now gpsd.socket
cgps          # shows the fix
```

GeoClue can't read gpsd directly. It can read NMEA from a Unix socket
(`nmea-socket` under `[network-nmea]`, see `man geoclue`), for example one
provided by `gps-share`.
