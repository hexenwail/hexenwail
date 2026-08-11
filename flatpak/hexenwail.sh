#!/bin/sh
# Hexenwail launcher — checks for game data before starting

BASEDIR="${HOME}"
DATADIR="${BASEDIR}/.hexen2/data1"
PAK="${DATADIR}/pak0.pak"

# Show the real host path when running inside Flatpak
# shellcheck disable=SC2088  # tilde is intentional: displayed to user, not used as a path
if [ -n "${FLATPAK_ID}" ]; then
    DISPLAY_PATH="~/.var/app/${FLATPAK_ID}/.hexen2/data1/"
else
    DISPLAY_PATH="~/.hexen2/data1/"
fi

# Offer the free 1997 demo rather than dead-ending someone who does not own
# the game.  get_demo.sh downloads it from the uHexen2 project and verifies a
# known hash; we ship no game content and could not (uhexen2-3vmk).  The
# sandbox already holds --share=network for LAN play and --persist=.hexen2 for
# exactly this directory, so no new permission is needed.  uhexen2-49ep.
offer_demo () {
    [ -x /app/bin/get_demo.sh ] || return 1
    command -v zenity >/dev/null 2>&1 || return 1

    zenity --question \
        --title="Hexenwail — Game Data Missing" \
        --text="Hexen II game data was not found.\n\nIf you own Hexen II, place pak0.pak and pak1.pak at:\n  ${DISPLAY_PATH}\n\nOtherwise, Hexenwail can download the free three-level demo\nRaven released in 1997 (13 MB) and install it there.\n\nDownload the demo now?" \
        --ok-label="Download demo" \
        --cancel-label="Quit" \
        2>/dev/null || return 1

    mkdir -p "${BASEDIR}/.hexen2" || return 1

    # Pulsating progress: the fetch reports percentages to stdout in a form
    # zenity cannot consume, and there is nothing useful to show between the
    # download and the checksum anyway.
    if /app/bin/get_demo.sh "${BASEDIR}/.hexen2" 2>&1 | \
       zenity --progress --pulsate --auto-close --no-cancel \
              --title="Hexenwail" --text="Downloading the Hexen II demo…" 2>/dev/null
    then
        [ -f "${PAK}" ] && return 0
    fi

    zenity --error --title="Hexenwail" \
        --text="The demo download did not complete.\n\nYou can retry, or place your own game data at:\n  ${DISPLAY_PATH}" \
        2>/dev/null
    return 1
}

if [ ! -f "${PAK}" ]; then
    if ! offer_demo; then
        # No zenity, no helper, or the user declined: say the same thing on
        # the console so a terminal launch is not left guessing.
        echo "Hexenwail: game data not found — place pak files at ${DISPLAY_PATH}" >&2
        exit 1
    fi
fi

exec /app/bin/glhexen2 "$@"
