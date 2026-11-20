#!/usr/bin/env bash
#
# surface-location-setup.sh - location ("GPS") for Surface devices on Linux.
#
#   ./surface-location-setup.sh            check what this device can do
#   sudo ./surface-location-setup.sh --fix install and configure GeoClue
#   ./surface-location-setup.sh --test     ask GeoClue for the location now
#
# Surface Pro 5 (2017): only the LTE model (1807) has a GPS/GLONASS receiver,
# in its Qualcomm Snapdragon X16 modem. The Wi-Fi model (1796) has none; it
# gets its location from nearby Wi-Fi networks, like on Windows.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-check}"
CONF=/etc/geoclue/conf.d/90-surface-location.conf

ok()   { printf '  \033[32m[ ok ]\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m[warn]\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; }
info() { printf '         %s\n' "$*"; }
hdr()  { printf '\n\033[1m%s\033[0m\n' "$*"; }

case "$MODE" in check|--fix|--test) ;; *) sed -n '3,11p' "$0"; exit 1;; esac
if [ "$MODE" = --fix ] && [ "$(id -u)" != 0 ]; then
    echo "--fix needs root: sudo $0 --fix" >&2
    exit 1
fi

if command -v pacman >/dev/null 2>&1; then DISTRO=arch
elif command -v apt-get >/dev/null 2>&1; then DISTRO=debian
elif command -v dnf >/dev/null 2>&1; then DISTRO=fedora
else DISTRO=other
fi

where_am_i() {
    for p in /usr/lib/geoclue-2.0/demos/where-am-i /usr/libexec/geoclue-2.0/demos/where-am-i \
             /usr/lib/*/geoclue-2.0/demos/where-am-i; do
        [ -x "$p" ] && { echo "$p"; return; }
    done
}

# ------------------------------------------------------------------ test
if [ "$MODE" = --test ]; then
    W="$(where_am_i)"
    [ -n "$W" ] || { bad "where-am-i not found (install geoclue / geoclue-2.0 demos)"; exit 1; }
    echo "Asking GeoClue for the location (up to 30 s)..."
    timeout 30 "$W" -t 25 -a 8 || { bad "no location; run $0 to see why"; exit 1; }
    exit 0
fi

# ---------------------------------------------------------------- device
hdr "Device"
PRODUCT="$(cat /sys/class/dmi/id/product_name 2>/dev/null || echo '?')"
SKU="$(cat /sys/class/dmi/id/product_sku 2>/dev/null || echo '?')"
info "$PRODUCT ($SKU)"
HAS_GPS=unknown
case "$SKU" in
    Surface_Pro_1796)
        HAS_GPS=no
        warn "Surface Pro 5 Wi-Fi model: there is no GPS receiver in this device"
        info "Location comes from nearby Wi-Fi networks (beaconDB), like on Windows."
        info "For satellite GPS, plug in a USB GPS receiver (see README)."
        ;;
    Surface_Pro_1807)
        HAS_GPS=yes
        ok "Surface Pro 5 LTE: GPS/GLONASS in the Snapdragon X16 modem (via ModemManager)"
        ;;
    *)
        info "not a Surface Pro 5: will use Wi-Fi positioning, and GPS if ModemManager finds one"
        ;;
esac

# ---------------------------------------------------------------- stack
hdr "Location service (GeoClue)"
if [ -n "$(where_am_i)" ] || [ -e /usr/lib/geoclue ] || [ -e /usr/libexec/geoclue ] || command -v geoclue >/dev/null 2>&1; then
    ok "GeoClue installed"
else
    bad "GeoClue not installed"
fi
if [ -e "$CONF" ]; then ok "Surface config installed ($CONF)"; else warn "Surface config not installed (run with --fix)"; fi

hdr "Wi-Fi positioning"
BACKEND=""
if systemctl is-active -q iwd 2>/dev/null; then BACKEND=iwd; fi
if systemctl is-active -q wpa_supplicant 2>/dev/null || pgrep -x wpa_supplicant >/dev/null 2>&1; then
    BACKEND=wpa_supplicant
fi
case "$BACKEND" in
    wpa_supplicant) ok "Wi-Fi runs through wpa_supplicant: GeoClue can scan nearby networks";;
    iwd)
        bad "Wi-Fi runs through iwd: GeoClue cannot read Wi-Fi scans from iwd"
        info "Switch NetworkManager to wpa_supplicant (your saved Wi-Fi passwords stay):"
        info "  sudo pacman -S --needed wpa_supplicant        (Arch)"
        info "  printf '[device]\\nwifi.backend=wpa_supplicant\\n' | sudo tee /etc/NetworkManager/conf.d/wifi-backend.conf"
        info "  sudo systemctl disable --now iwd; sudo systemctl restart NetworkManager"
        ;;
    *) warn "could not see wpa_supplicant running - Wi-Fi positioning needs it";;
esac
if command -v rfkill >/dev/null && rfkill list wifi 2>/dev/null | grep -q "blocked: yes"; then
    warn "Wi-Fi is switched off (rfkill): Wi-Fi positioning needs it on (it doesn't need to be connected)"
fi

hdr "GPS (modem)"
if [ "$HAS_GPS" = no ]; then
    info "not applicable on this model"
elif command -v mmcli >/dev/null 2>&1; then
    MODEMS="$(mmcli -L 2>/dev/null | grep -o '/Modem/[0-9]*' || true)"
    if [ -n "$MODEMS" ]; then
        for m in $MODEMS; do
            ok "ModemManager modem ${m##*/}: $(mmcli -m "${m##*/}" 2>/dev/null | sed -n 's/.*model: //p' | head -1)"
            mmcli -m "${m##*/}" --location-status 2>/dev/null | grep -iE "capabilities|enabled" | sed 's/^/           /'
        done
    else
        warn "ModemManager sees no modem (is ModemManager running? sudo systemctl enable --now ModemManager)"
    fi
else
    warn "ModemManager (mmcli) not installed"
fi

# ------------------------------------------------------------------- fix
if [ "$MODE" = --fix ]; then
    hdr "Applying fixes"
    pkgs_arch=(geoclue)
    pkgs_deb=(geoclue-2.0)
    pkgs_fed=(geoclue2)
    if [ "$HAS_GPS" != no ]; then
        pkgs_arch+=(modemmanager); pkgs_deb+=(modemmanager); pkgs_fed+=(ModemManager)
    fi
    case "$DISTRO" in
        arch)   pacman -S --needed --noconfirm "${pkgs_arch[@]}" || warn "package install failed";;
        debian) apt-get install -y "${pkgs_deb[@]}" || warn "package install failed";;
        fedora) dnf install -y "${pkgs_fed[@]}" || warn "package install failed";;
        *) warn "install GeoClue (and ModemManager for GPS) yourself";;
    esac

    install -D -m 0644 "$SCRIPT_DIR/90-surface-location.conf" "$CONF"
    if [ "$HAS_GPS" != no ]; then
        sed -i '/^\[modem-gps\]/,/^\[/ s/^enable=false/enable=true/' "$CONF"
        systemctl enable --now ModemManager >/dev/null 2>&1 || warn "could not start ModemManager"
        ok "GPS through ModemManager enabled"
    fi
    ok "installed $CONF"
    # GeoClue is D-Bus activated; restart it so the new config applies.
    systemctl restart geoclue 2>/dev/null || true
    ok "GeoClue restarted"
    info "Test with: $0 --test"
    info "Desktop apps (GNOME Maps, Firefox, weather, automatic time zone) ask for"
    info "permission the first time; allow location in your desktop's privacy settings."
fi
