Chumby One (falconwing) buildroot image - config partition
==========================================================

Edit these files from any computer, put the card back, and power on.
They are applied on every boot.

network-config   WiFi network (SSID + password). Pi Imager style.
user-data        hostname, ssh password login on/off, root password hash.
authorized_keys  (optional, create it) SSH public keys for root.
wpa_supplicant.conf  (optional, create it) used verbatim instead of
                 network-config, for anything fancier than WPA-PSK.

SSH: ssh root@<ip>   default password: root
The device asks DHCP for its hostname, so ssh root@chumby may work if
your router registers DHCP names in DNS.

FM radio: ssh in and run "radio on 100.3" (see "radio" for options).
fmradio_xclk     (optional, create an empty file) only if the radio tunes
                 but receives nothing: clocks the tuner from the SoC.
