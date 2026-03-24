#!/bin/sh
# Hexenwail launcher — checks for game data before starting

DATADIR="${HOME}/.hexen2/data1"
PAK="${DATADIR}/pak0.pak"

if [ ! -f "${PAK}" ]; then
    zenity --error \
        --title="Hexenwail — Game Data Missing" \
        --text="Hexen II game data not found.\n\nPlace your game data files at:\n  ~/.hexen2/data1/\n\nRequired files: pak0.pak, pak1.pak\n\nObtain these from your Hexen II installation or GOG/Steam." \
        2>/dev/null || \
    echo "Hexenwail: game data not found at ${DATADIR}" >&2
    exit 1
fi

exec /app/bin/glhexen2 "$@"
