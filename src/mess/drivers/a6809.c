/***************************************************************************

    Acorn 6809

    12/05/2009 Skeleton driver.

    Acorn System 3 update?
    http://acorn.chriswhy.co.uk/8bit_Upgrades/Acorn_6809_CPU.html

****************************************************************************/

#include "emu.h"
#include "cpu/m6809/m6809.h"
#include "machine/6522via.h"
#include "video/saa5050.h"
#include "video/mc6845.h"

static ADDRESS_MAP_START(a6809_mem, ADDRESS_SPACE_PROGRAM, 8)
	ADDRESS_MAP_UNMAP_HIGH
	AM_RANGE(0x0000,0x03ff) AM_RAM
	AM_RANGE(0x0400,0x07ff) AM_DEVREADWRITE("saa5050", saa5050_videoram_r, saa5050_videoram_w)	
	AM_RANGE(0x0800,0x0800) AM_DEVWRITE("mc6845", mc6845_address_w)
	AM_RANGE(0x0801,0x0801) AM_DEVREADWRITE("mc6845", mc6845_register_r , mc6845_register_w)
	AM_RANGE(0x0900,0x090f) AM_MIRROR(0xf0)AM_DEVREADWRITE("via", via_r, via_w)
	AM_RANGE(0xf000,0xf7ff) AM_ROM // optional ROM
	AM_RANGE(0xf800,0xffff) AM_ROM
ADDRESS_MAP_END

static ADDRESS_MAP_START( a6809_io , ADDRESS_SPACE_IO, 8)
	ADDRESS_MAP_UNMAP_HIGH
ADDRESS_MAP_END

/* Input ports */
static INPUT_PORTS_START( a6809 )
INPUT_PORTS_END


static MACHINE_RESET(a6809)
{
}

static VIDEO_UPDATE( a6809 )
{
	running_device *saa5050 = screen->machine->device("saa5050");

	saa5050_update(saa5050, bitmap, NULL);
	return 0;
}

static MC6845_UPDATE_ROW( a6809_update_row )
{
}

static const mc6845_interface a6809_crtc6845_interface =
{
	"screen",
	6 /*?*/,
	NULL,
	a6809_update_row,
	NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	NULL
};

static const saa5050_interface a6809_saa5050_intf =
{
	"screen",
	0,	/* starting gfxnum */
	40, 25, 40,  /* x, y, size */
      0 	/* rev y order */
};

static const via6522_interface via_intf =
{
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_NULL,
	DEVCB_CPU_INPUT_LINE("maincpu", INPUT_LINE_IRQ0)
};

static MACHINE_DRIVER_START( a6809 )
	/* basic machine hardware */
	MDRV_CPU_ADD("maincpu",M6809E, XTAL_4MHz)
	MDRV_CPU_PROGRAM_MAP(a6809_mem)
	MDRV_CPU_IO_MAP(a6809_io)

	MDRV_MACHINE_RESET(a6809)

	/* video hardware */
	MDRV_SCREEN_ADD("screen", RASTER)
	MDRV_SCREEN_REFRESH_RATE(50)
	MDRV_SCREEN_VBLANK_TIME(ATTOSECONDS_IN_USEC(SAA5050_VBLANK))

	MDRV_SCREEN_FORMAT(BITMAP_FORMAT_INDEXED16)
	MDRV_SCREEN_SIZE(40 * 6, 25 * 10)
	MDRV_SCREEN_VISIBLE_AREA(0, 40 * 6 - 1, 0, 25 * 10 - 1)
	MDRV_GFXDECODE(saa5050)
	MDRV_PALETTE_LENGTH(128)
	MDRV_PALETTE_INIT(saa5050)

	MDRV_SAA5050_ADD("saa5050", a6809_saa5050_intf)

	MDRV_VIDEO_UPDATE(a6809)
	
	MDRV_VIA6522_ADD("via", XTAL_4MHz / 4, via_intf)
	
	MDRV_MC6845_ADD("mc6845", MC6845, XTAL_4MHz / 2, a6809_crtc6845_interface)
MACHINE_DRIVER_END

/* ROM definition */
ROM_START( a6809 )
	ROM_REGION( 0x10000, "maincpu", 0 )
	ROM_LOAD( "acorn6809.bin", 0xf800, 0x0800, CRC(5fa5b632) SHA1(b14a884bf82a7a8c23bc03c2e112728dd1a74896) )

	/* This looks like some sort of prom */
	ROM_REGION( 0x100, "proms", 0 )
	ROM_LOAD( "acorn6809.ic11", 0x0000, 0x0100, CRC(7908317d) SHA1(e0f1e5bd3a8598d3b62bc432dd1f3892ed7e66d8) )
	
	/* This is SAA5050 so same char def is taken as in p2000t driver */
	ROM_REGION(0x01000, "gfx1",0)
	ROM_LOAD("p2000.chr", 0x0140, 0x08c0, BAD_DUMP CRC(78c17e3e) SHA1(4e1c59dc484505de1dc0b1ba7e5f70a54b0d4ccc))
ROM_END

/* Driver */

/*    YEAR   NAME    PARENT  COMPAT   MACHINE    INPUT    INIT    COMPANY   FULLNAME       FLAGS */
COMP( 1980?, a6809,  0,      0,       a6809,     a6809,   0,      "Acorn",  "6809",        GAME_NOT_WORKING | GAME_NO_SOUND )
