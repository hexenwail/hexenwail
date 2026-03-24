#!/bin/sh
# Hexenwail launcher — checks for game data before starting

DATADIR="${HOME}/.hexen2/data1"
PAK="${DATADIR}/pak0.pak"

# Show the real host path when running inside Flatpak
if [ -n "${FLATPAK_ID}" ]; then
    DISPLAY_PATH="~/.var/app/${FLATPAK_ID}/.hexen2/data1/"
else
    DISPLAY_PATH="~/.hexen2/data1/"
fi

if [ ! -f "${PAK}" ]; then
    zenity --error \
        --title="Hexenwail — Game Data Missing" \
        --text="Hexen II game data not found.\n\nPlace your game data files at:\n  ${DISPLAY_PATH}\n\nRequired files: pak0.pak, pak1.pak\n\nObtain these from your Hexen II installation or GOG/Steam." \
        2>/dev/null || \
    echo "Hexenwail: game data not found — place pak files at ${DISPLAY_PATH}" >&2
    exit 1
fi

exec /app/bin/glhexen2 "$@"
