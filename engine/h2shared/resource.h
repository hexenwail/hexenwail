// resource.h -- Windows resource header

#ifndef __HX2_RESOURCE_H
#define __HX2_RESOURCE_H

#define IDC_STATIC	(-1)

#define IDS_STRING1	1
#define IDI_ICON2	1
#define IDD_DIALOG1	108
#define IDD_PROGRESS	109
#define IDB_HXBITMAP	112
#define IDC_PROGRESS	1000

// Icon file to use
//
// One executable serves Hexen II, the mission pack and HexenWorld, so it has
// one icon: the Hammer of Thyrion "XII" art, generated from
// resource/hexenwail.svg by tools/gen_icons.sh (16 to 256 px).  The older
// per-game hexen2/h2mp/hexenworld .ico files stay as upstream shipped them.
#define RES_ICONFILE	"../resource/hexenwail.ico"

// String
//
#if defined(H2W)
#define SPLASH_STR	"Starting HexenWorld..."
#define RES_STRING	"HexenWorld"
//#elif defined(H2MP)
//#define SPLASH_STR	"Starting Hexen II+..."
//#define RES_STRING	"HexenII"
#else
#define SPLASH_STR	"Starting Hexen II..."
#define RES_STRING	"HexenII"
#endif

// Bitmap
//
#if defined(H2W)
#define SPLASH_BMP	"../../resource/hexenworld.bmp"
#else
#define SPLASH_BMP	"../resource/hexen2.bmp"
#endif

#endif	/* __HX2_RESOURCE_H */

