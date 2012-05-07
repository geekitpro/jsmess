/****************************************************************************

    MESS Driver for TI-99/4 and TI-99/4A Home Computers.
    Raphael Nabet, 1999-2003.

    TI99/4 info:

    Similar to TI99/4a, except for the following:
    * tms9918/9928 has no bitmap mode
    * smaller, 40-key keyboard
    * many small differences in the contents of system ROMs

    Historical notes: TI made several last minute design changes.
    * TI99/4 prototypes had an extra port for an I/R joystick and keypad interface.
    * early TI99/4 prototypes were designed for a tms9985, not a tms9900.

    Emulation architecture:
    (also see datamux.c, peribox.c)

              +---- video (upper 8 bits of databus)
              |
    CPU       |
    TMS9900 ==##== datamux --------+------ peribox ----------- [8] slots with peripheral devices
       |      ||   (16->8)         |
       |      ||                   +------ Console GROMs (0, 1, 2)
       |      ##=== Console ROM    |
    TMS9901   ||                   +------ gromport (cartridge port)
     (I/O)    ##=== 256 byte RAM   |
              ||                   +------ sound
              ##=== 32KiB RAM
                    unofficial mod
                    (16 bit)

    Michael Zapf

    February 2012: Rewritten as class

*****************************************************************************/


#include "emu.h"
#include "cpu/tms9900/tms9900.h"
#include "sound/sn76496.h"

#include "sound/wave.h"
#include "video/v9938.h"
#include "machine/tms9901.h"
#include "imagedev/cassette.h"

#include "machine/ti99/videowrp.h"
#include "machine/ti99/datamux.h"
#include "machine/ti99/grom.h"
#include "machine/ti99/gromport.h"
#include "machine/ti99/mecmouse.h"
#include "machine/ti99/handset.h"
#include "machine/ti99/peribox.h"

#define LOG logerror
#define VERBOSE 1

/*
    The console.
*/
class ti99_4x : public driver_device
{
public:
	ti99_4x(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag) { }

	// CRU (Communication Register Unit) handling
	DECLARE_READ8_MEMBER(cruread);
	DECLARE_WRITE8_MEMBER(cruwrite);

	// Forwarding interrupts to the CPU or CRU
	DECLARE_WRITE_LINE_MEMBER( console_ready );
	DECLARE_WRITE_LINE_MEMBER( set_tms9901_INT2 );
	DECLARE_WRITE_LINE_MEMBER( set_tms9901_INT12 );
	void set_tms9901_INT2_from_v9938(v99x8_device &vdp, int state);
	DECLARE_WRITE_LINE_MEMBER( extint );
	DECLARE_WRITE_LINE_MEMBER( notconnected );

	// Some values to keep
	int					m_mode;
	device_t			*m_cpu;
	tms9901_device		*m_tms9901;
	gromport_device		*m_gromport;
	peribox_device		*m_peribox;
	mecmouse_device 	*m_mecmouse;
	ti99_handset_device *m_handset;

	// Connections with the system interface TMS9901
	DECLARE_READ8_MEMBER(read_by_9901);
	DECLARE_WRITE_LINE_MEMBER(keyC0);
	DECLARE_WRITE_LINE_MEMBER(keyC1);
	DECLARE_WRITE_LINE_MEMBER(keyC2);
	DECLARE_WRITE_LINE_MEMBER(cs1_motor);
	DECLARE_WRITE_LINE_MEMBER(audio_gate);
	DECLARE_WRITE_LINE_MEMBER(cassette_output);
	DECLARE_WRITE8_MEMBER(tms9901_interrupt);
	DECLARE_WRITE_LINE_MEMBER(handset_ack);
	DECLARE_WRITE_LINE_MEMBER(cs2_motor);
	DECLARE_WRITE_LINE_MEMBER(alphaW);

private:
	void				set_key(int number, int data);
	int					m_keyboard_column;
	int					m_alphalock_line;
};

/*
    Memory map
*/
static ADDRESS_MAP_START(memmap, AS_PROGRAM, 16, ti99_4x)
	ADDRESS_MAP_GLOBAL_MASK(0xffff)
	AM_RANGE(0x0000, 0x1fff) AM_ROM										/*system ROM*/
	AM_RANGE(0x8000, 0x80ff) AM_MIRROR(0x0300) AM_RAM			/*RAM PAD, mirrored 4 times*/
	AM_RANGE(0x8800, 0x8bff) AM_DEVREADWRITE(VIDEO_SYSTEM_TAG, ti_std_video_device, read16, nowrite)	/*vdp read*/
	AM_RANGE(0x8C00, 0x8fff) AM_DEVREADWRITE(VIDEO_SYSTEM_TAG, ti_std_video_device, noread, write16)	/*vdp write*/
	AM_RANGE(0x0000, 0xffff) AM_DEVREADWRITE(DATAMUX_TAG, ti99_datamux_device, read, write)
ADDRESS_MAP_END

static ADDRESS_MAP_START(memmap_4ev, AS_PROGRAM, 16, ti99_4x)
	ADDRESS_MAP_GLOBAL_MASK(0xffff)
	AM_RANGE(0x0000, 0x1fff) AM_ROM										/*system ROM*/
	AM_RANGE(0x8000, 0x80ff) AM_MIRROR(0x0300) AM_RAM			/*RAM PAD, mirrored 4 times*/
	AM_RANGE(0x8800, 0x8bff) AM_DEVREADWRITE(VIDEO_SYSTEM_TAG, ti_exp_video_device, read16, nowrite)	/*vdp read*/
	AM_RANGE(0x8C00, 0x8fff) AM_DEVREADWRITE(VIDEO_SYSTEM_TAG, ti_exp_video_device, noread, write16)	/*vdp write*/
	AM_RANGE(0x0000, 0xffff) AM_DEVREADWRITE(DATAMUX_TAG, ti99_datamux_device, read, write)
ADDRESS_MAP_END

/*
    CRU map
    TMS9900 CRU address space is 12 bits wide, attached to A3-A14, A0-A2 must
    be 000 (other values for external commands like RSET, LREX, CKON...),
    A15 is used as CRUOUT
    The TMS9901 is incompletely decoded
    ---0 00xx xxcc ccc0
    causing 16 mirrors (0000, 0040, 0080, 00c0, ... , 03c0)

    Reading is done by transfering 8 successive bits, so addresses refer to
    8 bit groups; writing, however, is done using output lines. The CRU base
    address in the ti99 systems is twice the bit address:

    (base=0, bit=0x10) == (base=0x20,bit=0)

    Read: 0000 - 003f translates to base addresses 0000 - 03fe
          0000 - 01ff is the complete CRU address space 0000 - 1ffe (for TMS9900)

    Write:0000 - 01ff corresponds to bit 0 of base address 0000 - 03fe
*/
static ADDRESS_MAP_START(cru_map, AS_IO, 8, ti99_4x)
	AM_RANGE(0x0000, 0x003f) AM_DEVREAD(TMS9901_TAG, tms9901_device, read)
	AM_RANGE(0x0000, 0x01ff) AM_READ(cruread)

	AM_RANGE(0x0000, 0x01ff) AM_DEVWRITE(TMS9901_TAG, tms9901_device, write)
	AM_RANGE(0x0000, 0x0fff) AM_WRITE(cruwrite)
ADDRESS_MAP_END


/*****************************************************************************
    Input ports
 ****************************************************************************/
/* TI99/4a: 48-key keyboard, plus two optional joysticks (2 shift keys) */
static INPUT_PORTS_START(ti99_4a)

	PORT_START( "MECMOUSE" )
	PORT_CONFNAME( 0x01, 0x00, "Mechatronics Mouse" )
		PORT_CONFSETTING(    0x00, DEF_STR( Off ) )
		PORT_CONFSETTING(    0x01, DEF_STR( On ) )

	/* 4 ports for keyboard and joystick */
	PORT_START("KEY0")	/* col 0 */
		PORT_BIT(0x0088, IP_ACTIVE_LOW, IPT_UNUSED)
		/* The original control key is located on the left, but we accept the right control key as well */
		PORT_BIT(0x0040, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("CTRL")      PORT_CODE(KEYCODE_LCONTROL) PORT_CODE(KEYCODE_RCONTROL)
		/* TI99/4a has a second shift key which maps the same */
		PORT_BIT(0x0020, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_LSHIFT) PORT_CODE(KEYCODE_RSHIFT) PORT_CHAR(UCHAR_SHIFT_1)
		/* The original function key is located on the right, but we accept the left alt key as well */
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("FCTN")      PORT_CODE(KEYCODE_RALT) PORT_CODE(KEYCODE_LALT) PORT_CHAR(UCHAR_SHIFT_2)
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_ENTER) PORT_CHAR(13)
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_SPACE) PORT_CHAR(' ')
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("= + QUIT")  PORT_CODE(KEYCODE_EQUALS) PORT_CHAR('=') PORT_CHAR('+') PORT_CHAR(UCHAR_MAMEKEY(F12))
				/* col 1 */
		PORT_BIT(0x8000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_X)     PORT_CHAR('x') PORT_CHAR('X') PORT_CHAR(UCHAR_MAMEKEY(DOWN))
		PORT_BIT(0x4000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_W)     PORT_CHAR('w') PORT_CHAR('W') PORT_CHAR('~')
		PORT_BIT(0x2000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_S)     PORT_CHAR('s') PORT_CHAR('S') PORT_CHAR(UCHAR_MAMEKEY(LEFT))
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_2)     PORT_CHAR('2') PORT_CHAR('@') PORT_CHAR(UCHAR_MAMEKEY(F2))
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("9 ( BACK")  PORT_CODE(KEYCODE_9) PORT_CHAR('9') PORT_CHAR('(') PORT_CHAR(UCHAR_MAMEKEY(F9))
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_O)     PORT_CHAR('o') PORT_CHAR('O') PORT_CHAR('\'')
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_L)     PORT_CHAR('l') PORT_CHAR('L')
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_STOP)  PORT_CHAR('.') PORT_CHAR('>')

	PORT_START("KEY1")	/* col 2 */
		PORT_BIT(0x0080, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_C)     PORT_CHAR('c') PORT_CHAR('C') PORT_CHAR('`')
		PORT_BIT(0x0040, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_E)     PORT_CHAR('e') PORT_CHAR('E') PORT_CHAR(UCHAR_MAMEKEY(UP))
		PORT_BIT(0x0020, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_D)     PORT_CHAR('d') PORT_CHAR('D') PORT_CHAR(UCHAR_MAMEKEY(RIGHT))
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("3 # ERASE") PORT_CODE(KEYCODE_3) PORT_CHAR('3') PORT_CHAR('#') PORT_CHAR(UCHAR_MAMEKEY(F3))
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("8 * REDO")  PORT_CODE(KEYCODE_8) PORT_CHAR('8') PORT_CHAR('*') PORT_CHAR(UCHAR_MAMEKEY(F8))
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_I)     PORT_CHAR('i') PORT_CHAR('I') PORT_CHAR('?')
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_K)     PORT_CHAR('k') PORT_CHAR('K')
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_COMMA) PORT_CHAR(',') PORT_CHAR('<')
				/* col 3 */
		PORT_BIT(0x8000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_V)     PORT_CHAR('v') PORT_CHAR('V')
		PORT_BIT(0x4000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_R)     PORT_CHAR('r') PORT_CHAR('R') PORT_CHAR('[')
		PORT_BIT(0x2000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_F)     PORT_CHAR('f') PORT_CHAR('F') PORT_CHAR('{')
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("4 $ CLEAR") PORT_CODE(KEYCODE_4) PORT_CHAR('4') PORT_CHAR('$') PORT_CHAR(UCHAR_MAMEKEY(F4))
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("7 & AID")   PORT_CODE(KEYCODE_7) PORT_CHAR('7') PORT_CHAR('&') PORT_CHAR(UCHAR_MAMEKEY(F7))
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_U)     PORT_CHAR('u') PORT_CHAR('U') PORT_CHAR('_')
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_J)     PORT_CHAR('j') PORT_CHAR('J')
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_M)     PORT_CHAR('m') PORT_CHAR('M')

	PORT_START("KEY2")	/* col 4 */
		PORT_BIT(0x0080, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_B)     PORT_CHAR('b') PORT_CHAR('B')
		PORT_BIT(0x0040, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_T)     PORT_CHAR('t') PORT_CHAR('T') PORT_CHAR(']')
		PORT_BIT(0x0020, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_G)     PORT_CHAR('g') PORT_CHAR('G') PORT_CHAR('}')
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("5 % BEGIN")  PORT_CODE(KEYCODE_5) PORT_CHAR('5') PORT_CHAR('%') PORT_CHAR(UCHAR_MAMEKEY(F5))
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("6 ^ PROC'D") PORT_CODE(KEYCODE_6) PORT_CHAR('6') PORT_CHAR('^') PORT_CHAR(UCHAR_MAMEKEY(F6))
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_Y)     PORT_CHAR('y') PORT_CHAR('Y')
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_H)     PORT_CHAR('h') PORT_CHAR('H')
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_N)     PORT_CHAR('n') PORT_CHAR('N')
				/* col 5 */
		PORT_BIT(0x8000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_Z)     PORT_CHAR('z') PORT_CHAR('Z') PORT_CHAR('\\')
		PORT_BIT(0x4000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_Q)     PORT_CHAR('q') PORT_CHAR('Q')
		PORT_BIT(0x2000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_A)     PORT_CHAR('a') PORT_CHAR('A') PORT_CHAR('|')
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_1)     PORT_CHAR('1') PORT_CHAR('!') PORT_CHAR(UCHAR_MAMEKEY(DEL))
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_0)     PORT_CHAR('0') PORT_CHAR(')') PORT_CHAR(UCHAR_MAMEKEY(F10))
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_P)     PORT_CHAR('p') PORT_CHAR('P') PORT_CHAR('\"')
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_COLON) PORT_CHAR(';') PORT_CHAR(':')
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_CODE(KEYCODE_SLASH) PORT_CHAR('/') PORT_CHAR('-')

	PORT_START("KEY3")	/* col 6: "wired handset 1" (= joystick 1) */
		PORT_BIT(0x00E0, IP_ACTIVE_LOW, IPT_UNUSED)
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_JOYSTICK_UP/*, "(1UP)", CODE_NONE, OSD_JOY_UP*/) PORT_PLAYER(1)
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN/*, "(1DOWN)", CODE_NONE, OSD_JOY_DOWN, 0*/) PORT_PLAYER(1)
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT/*, "(1RIGHT)", CODE_NONE, OSD_JOY_RIGHT, 0*/) PORT_PLAYER(1)
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT/*, "(1LEFT)", CODE_NONE, OSD_JOY_LEFT, 0*/) PORT_PLAYER(1)
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_BUTTON1/*, "(1FIRE)", CODE_NONE, OSD_JOY_FIRE, 0*/) PORT_PLAYER(1)
			/* col 7: "wired handset 2" (= joystick 2) */
		PORT_BIT(0xE000, IP_ACTIVE_LOW, IPT_UNUSED)
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_JOYSTICK_UP/*, "(2UP)", CODE_NONE, OSD_JOY2_UP, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN/*, "(2DOWN)", CODE_NONE, OSD_JOY2_DOWN, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT/*, "(2RIGHT)", CODE_NONE, OSD_JOY2_RIGHT, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT/*, "(2LEFT)", CODE_NONE, OSD_JOY2_LEFT, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_BUTTON1/*, "(2FIRE)", CODE_NONE, OSD_JOY2_FIRE, 0*/) PORT_PLAYER(2)


	PORT_START("ALPHA")	/* one more port for Alpha line */
		PORT_BIT(0x0010, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("Alpha Lock") PORT_CODE(KEYCODE_CAPSLOCK) PORT_TOGGLE

INPUT_PORTS_END

static INPUT_PORTS_START(ti99_4)
	PORT_START( "MECMOUSE" )
	PORT_CONFNAME( 0x01, 0x00, "Mechatronics Mouse" )
		PORT_CONFSETTING(    0x00, DEF_STR( Off ) )
		PORT_CONFSETTING(    0x01, DEF_STR( On ) )

	PORT_START( "HANDSET" )
	PORT_CONFNAME( 0x01, 0x00, "IR Handset" )
		PORT_CONFSETTING(    0x00, DEF_STR( Off ) )
		PORT_CONFSETTING(    0x01, DEF_STR( On ) )

	/* 4 ports for keyboard and joystick */
	PORT_START("KEY0")	/* col 0 */
		PORT_BIT(0x0080, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("1 !") PORT_CODE(KEYCODE_1) PORT_CHAR('1') PORT_CHAR('!')
		PORT_BIT(0x0040, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("Q QUIT") PORT_CODE(KEYCODE_Q) PORT_CHAR('Q') PORT_CHAR(UCHAR_MAMEKEY(F12))
		/* TI99/4 has a second space key which maps the same */
		PORT_BIT(0x0020, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("SPACE") PORT_CODE(KEYCODE_SPACE) PORT_CODE(KEYCODE_CAPSLOCK) PORT_CHAR(' ')
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("SHIFT") PORT_CODE(KEYCODE_LSHIFT) PORT_CHAR(UCHAR_SHIFT_1)
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("0 )") PORT_CODE(KEYCODE_0) PORT_CHAR('0') PORT_CHAR(')')
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("P \"") PORT_CODE(KEYCODE_P) PORT_CHAR('P') PORT_CHAR('"')
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("L =") PORT_CODE(KEYCODE_L) PORT_CHAR('L') PORT_CHAR('=')
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("ENTER") PORT_CODE(KEYCODE_ENTER) PORT_CHAR(13)
				/* col 1 */
		PORT_BIT(0x8000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("2 @") PORT_CODE(KEYCODE_2) PORT_CHAR('2') PORT_CHAR('@')
		PORT_BIT(0x4000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("W BEGIN") PORT_CODE(KEYCODE_W) PORT_CHAR('W') PORT_CHAR(UCHAR_MAMEKEY(F5))
		PORT_BIT(0x2000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("A AID") PORT_CODE(KEYCODE_A) PORT_CHAR('A') PORT_CHAR(UCHAR_MAMEKEY(F7))
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("Z BACK") PORT_CODE(KEYCODE_Z) PORT_CHAR('Z') PORT_CHAR(UCHAR_MAMEKEY(F9))
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("9 (") PORT_CODE(KEYCODE_9) PORT_CHAR('9') PORT_CHAR('(')
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("O +") PORT_CODE(KEYCODE_O) PORT_CHAR('O') PORT_CHAR('+')
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("K /") PORT_CODE(KEYCODE_K) PORT_CHAR('K') PORT_CHAR('/')
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME(", .") PORT_CODE(KEYCODE_STOP) PORT_CHAR(',') PORT_CHAR('.')

	PORT_START("KEY1")	/* col 2 */
		PORT_BIT(0x0080, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("3 #") PORT_CODE(KEYCODE_3) PORT_CHAR('3') PORT_CHAR('#')
		PORT_BIT(0x0040, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("E UP") PORT_CODE(KEYCODE_E) PORT_CHAR('E') PORT_CHAR(UCHAR_MAMEKEY(UP))
		PORT_BIT(0x0020, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("S LEFT") PORT_CODE(KEYCODE_S) PORT_CHAR('S') PORT_CHAR(UCHAR_MAMEKEY(LEFT))
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("X DOWN") PORT_CODE(KEYCODE_X) PORT_CHAR('X') PORT_CHAR(UCHAR_MAMEKEY(DOWN))
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("8 *") PORT_CODE(KEYCODE_8) PORT_CHAR('8') PORT_CHAR('*')
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("I -") PORT_CODE(KEYCODE_I) PORT_CHAR('I') PORT_CHAR('-')
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("J ^") PORT_CODE(KEYCODE_J) PORT_CHAR('J') PORT_CHAR('^')
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("M ;") PORT_CODE(KEYCODE_M) PORT_CHAR('M') PORT_CHAR(';')
				/* col 3 */
		PORT_BIT(0x8000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("4 $") PORT_CODE(KEYCODE_4) PORT_CHAR('4') PORT_CHAR('$')
		PORT_BIT(0x4000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("R REDO") PORT_CODE(KEYCODE_R) PORT_CHAR('R') PORT_CHAR(UCHAR_MAMEKEY(F8))
		PORT_BIT(0x2000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("D RIGHT") PORT_CODE(KEYCODE_D) PORT_CHAR('D') PORT_CHAR(UCHAR_MAMEKEY(RIGHT))
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("C CLEAR") PORT_CODE(KEYCODE_C) PORT_CHAR('C') PORT_CHAR(UCHAR_MAMEKEY(F4))
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("7 &") PORT_CODE(KEYCODE_7) PORT_CHAR('7') PORT_CHAR('&')
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("U _") PORT_CODE(KEYCODE_U) PORT_CHAR('U') PORT_CHAR('_')
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("H <") PORT_CODE(KEYCODE_H) PORT_CHAR('H') PORT_CHAR('<')
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("N :") PORT_CODE(KEYCODE_N) PORT_CHAR('N') PORT_CHAR(':')

	PORT_START("KEY2")	/* col 4 */
		PORT_BIT(0x0080, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("5 %") PORT_CODE(KEYCODE_5) PORT_CHAR('5') PORT_CHAR('%')
		PORT_BIT(0x0040, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("T ERASE") PORT_CODE(KEYCODE_T) PORT_CHAR('T') PORT_CHAR(UCHAR_MAMEKEY(F3))
		PORT_BIT(0x0020, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("F DEL") PORT_CODE(KEYCODE_F) PORT_CHAR('F') PORT_CHAR(UCHAR_MAMEKEY(DEL))
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("V PROC'D") PORT_CODE(KEYCODE_V) PORT_CHAR('V') PORT_CHAR(UCHAR_MAMEKEY(F6))
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("6 ^") PORT_CODE(KEYCODE_6) PORT_CHAR('6') PORT_CHAR('\'')
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("Y >") PORT_CODE(KEYCODE_Y) PORT_CHAR('Y') PORT_CHAR('>')
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("G INS") PORT_CODE(KEYCODE_G) PORT_CHAR('G') PORT_CHAR(UCHAR_MAMEKEY(F2))
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_KEYBOARD) PORT_NAME("B ?") PORT_CODE(KEYCODE_B) PORT_CHAR('B') PORT_CHAR('?')
				/* col 5: "wired handset 1" (= joystick 1) */
		PORT_BIT(0xE000, IP_ACTIVE_LOW, IPT_UNUSED)
		PORT_BIT(0x1000, IP_ACTIVE_LOW, IPT_JOYSTICK_UP/*, "(1UP)", CODE_NONE, OSD_JOY_UP*/) PORT_PLAYER(1)
		PORT_BIT(0x0800, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN/*, "(1DOWN)", CODE_NONE, OSD_JOY_DOWN, 0*/) PORT_PLAYER(1)
		PORT_BIT(0x0400, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT/*, "(1RIGHT)", CODE_NONE, OSD_JOY_RIGHT, 0*/) PORT_PLAYER(1)
		PORT_BIT(0x0200, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT/*, "(1LEFT)", CODE_NONE, OSD_JOY_LEFT, 0*/) PORT_PLAYER(1)
		PORT_BIT(0x0100, IP_ACTIVE_LOW, IPT_BUTTON1/*, "(1FIRE)", CODE_NONE, OSD_JOY_FIRE, 0*/) PORT_PLAYER(1)

	PORT_START("KEY3")	/* col 6: "wired handset 2" (= joystick 2) */
		PORT_BIT(0x00E0, IP_ACTIVE_LOW, IPT_UNUSED)
		PORT_BIT(0x0010, IP_ACTIVE_LOW, IPT_JOYSTICK_UP/*, "(2UP)", CODE_NONE, OSD_JOY2_UP, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0008, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN/*, "(2DOWN)", CODE_NONE, OSD_JOY2_DOWN, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0004, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT/*, "(2RIGHT)", CODE_NONE, OSD_JOY2_RIGHT, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0002, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT/*, "(2LEFT)", CODE_NONE, OSD_JOY2_LEFT, 0*/) PORT_PLAYER(2)
		PORT_BIT(0x0001, IP_ACTIVE_LOW, IPT_BUTTON1/*, "(2FIRE)", CODE_NONE, OSD_JOY2_FIRE, 0*/) PORT_PLAYER(2)
				/* col 7: never used (selects IR remote handset instead) */
		/*PORT_BITX(0xFF00, IP_ACTIVE_LOW, IPT_UNUSED, DEF_STR( Unused ), CODE_NONE, CODE_NONE)*/
INPUT_PORTS_END

/*****************************************************************************
    Components
******************************************************************************/

static GROM_CONFIG(grom0_config)
{
	false, 0, region_grom, 0x0000, 0x1800, false, DEVCB_DRIVER_LINE_MEMBER(ti99_4x, console_ready)
};

static GROM_CONFIG(grom1_config)
{
	false, 1, region_grom, 0x2000, 0x1800, false, DEVCB_DRIVER_LINE_MEMBER(ti99_4x, console_ready)
};

static GROM_CONFIG(grom2_config)
{
	false, 2, region_grom, 0x4000, 0x1800, false, DEVCB_DRIVER_LINE_MEMBER(ti99_4x, console_ready)
};

static GROMPORT_CONFIG(console_cartslot)
{
	DEVCB_DRIVER_LINE_MEMBER(ti99_4x, console_ready)
};

static PERIBOX_CONFIG( peribox_conf )
{
	DEVCB_DRIVER_LINE_MEMBER(ti99_4x, extint),			// INTA
	DEVCB_DRIVER_LINE_MEMBER(ti99_4x, notconnected),	// INTB
	DEVCB_DRIVER_LINE_MEMBER(ti99_4x, console_ready),	// READY
	0x70000												// Address bus prefix (AMA/AMB/AMC)
};

READ8_MEMBER( ti99_4x::cruread )
{
//  if (VERBOSE>6) LOG("read access to CRU address %04x\n", offset << 4);
	UINT8 value = 0;

	// Similar to the bus8z_devices, just let the gromport and the p-box
	// decide whether they want to change the value at the CRU address
	// Also, we translate the bit addresses to base addresses
	m_gromport->crureadz(offset<<4, &value);
	m_peribox->crureadz(offset<<4, &value);

	return value;
}

WRITE8_MEMBER( ti99_4x::cruwrite )
{
//  if (VERBOSE>6) LOG("write access to CRU address %04x\n", offset << 1);
	m_gromport->cruwrite(offset<<1, data);
	m_peribox->cruwrite(offset<<1, data);
}

/***************************************************************************
    TI99/4x-specific tms9901 I/O handlers

    See mess/machine/tms9901.c for generic tms9901 CRU handlers.

    TMS9901 interrupt handling on a TI99/4(a).

    TI99/4(a) uses the following interrupts:
    INT1: external interrupt (used by RS232 controller, for instance)
    INT2: VDP interrupt
    TMS9901 timer interrupt (overrides INT3)
    INT12: handset interrupt (only on a TI-99/4 with the handset prototypes)

    Three (occasionally four) interrupts are used by the system (INT1, INT2,
    timer, and INT12 on a TI-99/4 with remote handset prototypes), out of 15/16
    possible interrupts.  Keyboard pins can be used as interrupt pins, too, but
    this is not emulated (it's a trick, anyway, and I don't know any program
    which uses it).

    When an interrupt line is set (and the corresponding bit in the interrupt mask is set),
    a level 1 interrupt is requested from the TMS9900.  This interrupt request lasts as long as
    the interrupt pin and the revelant bit in the interrupt mask are set.

***************************************************************************/


static const char *const keynames[] = { "KEY0", "KEY1", "KEY2", "KEY3" };

READ8_MEMBER( ti99_4x::read_by_9901 )
{
	int answer=0;

	switch (offset & 0x03)
	{
	case TMS9901_CB_INT7:
		//
		// Read pins INT3*-INT7* of TI99's 9901.
		// bit 1: INT1 status (interrupt; not set at this place)
		// bit 2: INT2 status (interrupt; not set at this place)
		// bit 3-7: keyboard status bits 0 to 4
		//
		if (m_mode==TI994)
		{
			if (m_keyboard_column == 7)
			{
				if (m_handset != NULL)
				{
					answer = (m_handset->poll_bus() << 3);
					answer |= 0x80;
				}
			}
			else
			{
				if ((m_mecmouse != NULL) && (m_keyboard_column == 6))
				{
					answer = m_mecmouse->get_values();
				}
				else
				{
					answer = ((ioport(keynames[m_keyboard_column >> 1])->read() >> ((m_keyboard_column & 1) * 8)) << 3) & 0xF8;
				}
			}
		}
		else
		{
			if ((m_mecmouse != NULL) && (m_keyboard_column == 7))
			{
				answer = m_mecmouse->get_values();
			}
			else
			{
				answer = ((ioport(keynames[m_keyboard_column >> 1])->read() >> ((m_keyboard_column & 1) * 8)) << 3) & 0xF8;
			}

			if ((m_mode != TI994) && (m_alphalock_line==false))
			{
				answer &= ~(ioport("ALPHA")->read() << 3);
			}
		}

		break;

	case TMS9901_INT8_INT15:
		if (m_keyboard_column == 7)
		{
			answer = 0x07;
		}
		else
		{
			answer = ((ioport(keynames[m_keyboard_column >> 1])->read() >> ((m_keyboard_column & 1) * 8)) >> 5) & 0x07;
		}
		break;

	case TMS9901_P0_P7:
		if (m_handset != NULL)
		{
			if (m_handset->get_clock()) answer |= 2;
		}
		break;

	case TMS9901_P8_P15:
		answer = 4;
		/* on systems without handset, the pin is pulled up to avoid spurious interrupts */
		if (m_handset != NULL)
		{
			if (m_handset->get_int()) answer = 0;
		}
		/* we don't take CS2 into account, as CS2 is a write-only unit */
		if ((machine().device<cassette_image_device>(CASSETTE_TAG))->input() > 0)
		{
			answer |= 8;
		}
		break;
	}
	return answer;
}

/*
    Handler for tms9901 P0 pin (handset data acknowledge)
*/
WRITE_LINE_MEMBER( ti99_4x::handset_ack )
{
	if (m_handset != NULL)
	{
		if (VERBOSE>7) LOG("ti99_4x: Set handset ack %d\n", state);
		m_handset->set_acknowledge(state);
	}
}

/*
    WRITE key column select (P2-P4), TI-99/4
*/
void ti99_4x::set_key(int number, int data)
{
	int index = (m_mode==TI994A)? 6 : 5;

	if (data != 0)
		m_keyboard_column |= 1 << number;
	else
		m_keyboard_column &= ~ (1 << number);

	if (m_mecmouse != NULL)
		m_mecmouse->select(m_keyboard_column, index, index+1);
}

WRITE_LINE_MEMBER( ti99_4x::keyC0 )
{
	set_key(0, state);
}

WRITE_LINE_MEMBER( ti99_4x::keyC1 )
{
	set_key(1, state);
}

WRITE_LINE_MEMBER( ti99_4x::keyC2 )
{
	set_key(2, state);
}

/*
    Select alpha lock line - TI99/4a only (P5)
*/
WRITE_LINE_MEMBER( ti99_4x::alphaW )
{
	m_alphalock_line = state;
}

/*
    Control CS1 tape unit motor (P6)
*/
WRITE_LINE_MEMBER( ti99_4x::cs1_motor )
{
	cassette_image_device *img = machine().device<cassette_image_device>(CASSETTE_TAG);
	img->change_state(state==ASSERT_LINE? CASSETTE_MOTOR_ENABLED : CASSETTE_MOTOR_DISABLED,CASSETTE_MASK_MOTOR);
}

/*
    Control CS2 tape unit motor (P7)
*/
WRITE_LINE_MEMBER( ti99_4x::cs2_motor )
{
	cassette_image_device *img = machine().device<cassette_image_device>(CASSETTE2_TAG);
	img->change_state(state==ASSERT_LINE? CASSETTE_MOTOR_ENABLED : CASSETTE_MOTOR_DISABLED,CASSETTE_MASK_MOTOR);
}

/*
    Audio gate (P8)
    Set to 1 before using tape: this enables the mixing of tape input sound
    with computer sound.
    We do not really need to emulate this as the tape recorder generates sound
    on its own.
*/
WRITE_LINE_MEMBER( ti99_4x::audio_gate )
{
}

/*
    Tape output (P9)
    I think polarity is correct, but don't take my word for it.
*/
WRITE_LINE_MEMBER( ti99_4x::cassette_output )
{
	machine().device<cassette_image_device>(CASSETTE_TAG)->output(state==ASSERT_LINE? +1 : -1);
	machine().device<cassette_image_device>(CASSETTE2_TAG)->output(state==ASSERT_LINE? +1 : -1);
}

WRITE8_MEMBER( ti99_4x::tms9901_interrupt )
{
	// offset contains the interrupt level (0-15)
	if (data==ASSERT_LINE)
	{
		// The TMS9901 should normally be connected with the CPU by 5 wires:
		// INTREQ* and IC0-IC3. The last four lines deliver the interrupt level.
		// On the TI-99 systems these IC lines are not used; the input lines
		// at the CPU are hardwired to level 1.
		device_execute(m_cpu)->set_input_line_and_vector(0, ASSERT_LINE, 1);
	}
	else
	{
		device_execute(m_cpu)->set_input_line(0, CLEAR_LINE);
	}
}

/*****************************************************************************/

/*
    set the state of TMS9901's INT2 (called by the tms9928 core)
*/
WRITE_LINE_MEMBER( ti99_4x::set_tms9901_INT2 )
{
	if (VERBOSE>6) LOG("ti99_4x: VDP int 2 on tms9901, level=%02x\n", state);
	m_tms9901->set_single_int(2, state);
}

void ti99_4x::set_tms9901_INT2_from_v9938(v99x8_device &vdp, int state)
{
	m_tms9901->set_single_int(2, state);
}

/*
    set the state of TMS9901's INT12 (called by the handset prototype of TI-99/4)
*/
WRITE_LINE_MEMBER( ti99_4x::set_tms9901_INT12)
{
	if (m_handset!=NULL) m_tms9901->set_single_int(12, state);
}

/***********************************************************
    Links to external devices
***********************************************************/


WRITE_LINE_MEMBER( ti99_4x::console_ready )
{
	if (VERBOSE>6) LOG("READY line set ... not yet connected, level=%02x\n", state);
}

WRITE_LINE_MEMBER( ti99_4x::extint )
{
	if (VERBOSE>6) LOG("EXTINT level = %02x\n", state);
	if (m_tms9901 != NULL)
		m_tms9901->set_single_int(1, state);
}

WRITE_LINE_MEMBER( ti99_4x::notconnected )
{
	if (VERBOSE>6) LOG("Setting a not connected line ... ignored\n");
}

/*****************************************************************************/

static TMS9928A_INTERFACE(ti99_4_tms9928a_interface)
{
	SCREEN_TAG,
	0x4000,
	DEVCB_DRIVER_LINE_MEMBER(ti99_4x, set_tms9901_INT2)
};

static TI99_HANDSET_INTERFACE(ti99_4_handset_interface)
{
	DEVCB_DRIVER_LINE_MEMBER(ti99_4x, set_tms9901_INT12)
};

/* TMS9901 setup. */
const tms9901_interface tms9901_wiring_ti99_4 =
{
	TMS9901_INT1 | TMS9901_INT2 | TMS9901_INTC,	/* only input pins whose state is always known */

	// read handler
	DEVCB_DRIVER_MEMBER(ti99_4x, read_by_9901),

	// write handlers
	{
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, handset_ack),
		DEVCB_NULL,
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, keyC0),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, keyC1),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, keyC2),
		DEVCB_NULL,
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, cs1_motor),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, cs2_motor),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, audio_gate),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, cassette_output),
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL
	},

	// interrupt handler
	DEVCB_DRIVER_MEMBER(ti99_4x, tms9901_interrupt)
};

const tms9901_interface tms9901_wiring_ti99_4a =
{
	TMS9901_INT1 | TMS9901_INT2 | TMS9901_INTC,

	// read handler
	DEVCB_DRIVER_MEMBER(ti99_4x, read_by_9901),

	// write handlers
	{
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, keyC0),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, keyC1),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, keyC2),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, alphaW),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, cs1_motor),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, cs2_motor),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, audio_gate),
		DEVCB_DRIVER_LINE_MEMBER(ti99_4x, cassette_output),
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL,
		DEVCB_NULL
	},

	DEVCB_DRIVER_MEMBER(ti99_4x, tms9901_interrupt)
};

/*
    Devices attached to the databus multiplexer. We cannot solve this with
    the common address maps since the multiplexer also inserts wait states
    that we want to emulate properly. Also, devices may reside on the same
    memory locations (like GROMs) and select themselves according to some
    inner state (e.g. GROMs have an own address counter and a given address
    area).
*/
static const dmux_device_list_entry dmux_devices[] =
{
	{ GROM0_TAG,     0x9800, 0xfc01, 0x0400, "GROMENA", 0x01, 0x00 },
	{ GROM1_TAG,     0x9800, 0xfc01, 0x0400, "GROMENA", 0x01, 0x00 },
	{ GROM2_TAG,     0x9800, 0xfc01, 0x0400, "GROMENA", 0x01, 0x00 },
	{ TISOUND_TAG,   0x8400, 0xfc01, 0x0000, NULL, 0, 0 },
	{ GROMPORT_TAG,  0x9800, 0xfc01, 0x0400, NULL, 0, 0 },
	{ GROMPORT_TAG,  0x6000, 0xe000, 0x0000, NULL, 0, 0 },
	{ PERIBOX_TAG,   0x0000, 0x0000, 0x0000, NULL, 0, 0 },  // Peribox needs all addresses
	{ NULL, 0, 0, 0, NULL, 0, 0  }
};

/******************************************************************************
    Machine definitions
******************************************************************************/

MACHINE_START( ti99_4 )
{
	ti99_4x *driver = machine.driver_data<ti99_4x>();
	driver->m_mode = TI994;

	driver->m_cpu = machine.device("maincpu");
	driver->m_tms9901 = static_cast<tms9901_device*>(machine.device(TMS9901_TAG));

	driver->m_gromport = static_cast<gromport_device*>(machine.device(GROMPORT_TAG));

	driver->m_peribox = static_cast<peribox_device*>(machine.device(PERIBOX_TAG));

	driver->m_peribox->senila(CLEAR_LINE);
	driver->m_peribox->senilb(CLEAR_LINE);
}

MACHINE_RESET( ti99_4 )
{
	ti99_4x *driver = machine.driver_data<ti99_4x>();
	if (machine.root_device().ioport("HANDSET")->read()==0x01)
		driver->m_handset = static_cast<ti99_handset_device*>(machine.device(HANDSET_TAG));
	else
		driver->m_handset = NULL;

	if (machine.root_device().ioport("MECMOUSE")->read()==0x01)
		driver->m_mecmouse = static_cast<mecmouse_device*>(machine.device(MECMOUSE_TAG));
	else
		driver->m_mecmouse = NULL;
}

/*
    TI-99/4 - the predecessor of the more popular TI-99/4A
*/
static MACHINE_CONFIG_START( ti99_4_60hz, ti99_4x )
	MCFG_CPU_ADD("maincpu", TMS9900, 3000000)
	MCFG_CPU_PROGRAM_MAP(memmap)
	MCFG_CPU_IO_MAP(cru_map)
	MCFG_MACHINE_START( ti99_4 )
	MCFG_MACHINE_RESET( ti99_4 )

	MCFG_TI_TMS991x_ADD_NTSC(VIDEO_SYSTEM_TAG, TMS9118, ti99_4_tms9928a_interface)

	/* Main board */
	MCFG_TMS9901_ADD(TMS9901_TAG, tms9901_wiring_ti99_4, 3000000)
	MCFG_DMUX_ADD( DATAMUX_TAG, dmux_devices )
	MCFG_TI99_GROMPORT_ADD( GROMPORT_TAG, console_cartslot )

	/* Software list */
	MCFG_SOFTWARE_LIST_ADD("cart_list_ti99", "ti99_cart")

	/* Peripheral expansion box */
	MCFG_PERIBOX_ADD( PERIBOX_TAG, peribox_conf )

	/* sound hardware */
	MCFG_SPEAKER_STANDARD_MONO("mono")
	MCFG_SOUND_ADD(TISOUNDCHIP_TAG, SN94624, 3579545/8)	/* 3.579545 MHz */
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.75)
	MCFG_TI_SOUND_ADD( TISOUND_TAG )

	/* Cassette drives */
	MCFG_CASSETTE_ADD( CASSETTE_TAG, default_cassette_interface )
	MCFG_CASSETTE_ADD( CASSETTE2_TAG, default_cassette_interface )

	MCFG_SOUND_WAVE_ADD(WAVE_TAG, CASSETTE_TAG)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.25)

	/* GROM devices */
	MCFG_GROM_ADD( GROM0_TAG, grom0_config )
	MCFG_GROM_ADD( GROM1_TAG, grom1_config )
	MCFG_GROM_ADD( GROM2_TAG, grom2_config )

	/* User controller */
	MCFG_MECMOUSE_ADD( MECMOUSE_TAG, 60 )
	MCFG_HANDSET_ADD( HANDSET_TAG, ti99_4_handset_interface, 60 )


MACHINE_CONFIG_END

static MACHINE_CONFIG_START( ti99_4_50hz, ti99_4x )
	MCFG_CPU_ADD("maincpu", TMS9900, 3000000)
	MCFG_CPU_PROGRAM_MAP(memmap)
	MCFG_CPU_IO_MAP(cru_map)
	MCFG_MACHINE_START( ti99_4 )
	MCFG_MACHINE_RESET( ti99_4 )

	/* video hardware */
	MCFG_TI_TMS991x_ADD_PAL(VIDEO_SYSTEM_TAG, TMS9929, ti99_4_tms9928a_interface)

	/* main board */
	MCFG_TMS9901_ADD(TMS9901_TAG, tms9901_wiring_ti99_4, 3000000)
	MCFG_DMUX_ADD( DATAMUX_TAG, dmux_devices )
	MCFG_TI99_GROMPORT_ADD( GROMPORT_TAG, console_cartslot )

	/* Software list */
	MCFG_SOFTWARE_LIST_ADD("cart_list_ti99", "ti99_cart")

	/* Peripheral expansion box */
	MCFG_PERIBOX_ADD( PERIBOX_TAG, peribox_conf )

	/* sound hardware */
	MCFG_SPEAKER_STANDARD_MONO("mono")
	MCFG_SOUND_ADD(TISOUNDCHIP_TAG, SN94624, 3579545/8)	/* 3.579545 MHz */
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.75)
	MCFG_TI_SOUND_ADD( TISOUND_TAG )

	/* Cassette drives */
	MCFG_CASSETTE_ADD( CASSETTE_TAG, default_cassette_interface )
	MCFG_CASSETTE_ADD( CASSETTE2_TAG, default_cassette_interface )

	MCFG_SOUND_WAVE_ADD(WAVE_TAG, CASSETTE_TAG)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.25)

	/* GROM devices */
	MCFG_GROM_ADD( GROM0_TAG, grom0_config )
	MCFG_GROM_ADD( GROM1_TAG, grom1_config )
	MCFG_GROM_ADD( GROM2_TAG, grom2_config )

	/* User controller */
	MCFG_MECMOUSE_ADD( MECMOUSE_TAG, 50 )
	MCFG_HANDSET_ADD( HANDSET_TAG, ti99_4_handset_interface, 50 )
MACHINE_CONFIG_END

/*
    TI-99/4A - replaced the 99/4
*/

MACHINE_START( ti99_4a )
{
	ti99_4x *driver = machine.driver_data<ti99_4x>();
	driver->m_mode = TI994A;

	driver->m_cpu = machine.device("maincpu");
	driver->m_tms9901 = static_cast<tms9901_device*>(machine.device(TMS9901_TAG));

	driver->m_mecmouse = static_cast<mecmouse_device*>(machine.device(MECMOUSE_TAG));
	driver->m_gromport = static_cast<gromport_device*>(machine.device(GROMPORT_TAG));
	driver->m_peribox = static_cast<peribox_device*>(machine.device(PERIBOX_TAG));

	driver->m_peribox->senila(CLEAR_LINE);
	driver->m_peribox->senilb(CLEAR_LINE);
}

MACHINE_RESET( ti99_4a )
{
	ti99_4x *driver = machine.driver_data<ti99_4x>();
	if (machine.root_device().ioport("MECMOUSE")->read()==0x01)
		driver->m_mecmouse = static_cast<mecmouse_device*>(machine.device(MECMOUSE_TAG));
	else
		driver->m_mecmouse = NULL;
}

static MACHINE_CONFIG_START( ti99_4a_60hz, ti99_4x )
	MCFG_CPU_ADD("maincpu", TMS9900, 3000000)
	MCFG_CPU_PROGRAM_MAP(memmap)
	MCFG_CPU_IO_MAP(cru_map)
	MCFG_MACHINE_START( ti99_4a )
	MCFG_MACHINE_RESET( ti99_4a )

	/* Video hardware */
	MCFG_TI_TMS991x_ADD_NTSC(VIDEO_SYSTEM_TAG, TMS9918A, ti99_4_tms9928a_interface)

	/* Main board */
	MCFG_TMS9901_ADD(TMS9901_TAG, tms9901_wiring_ti99_4a, 3000000)
	MCFG_DMUX_ADD( DATAMUX_TAG, dmux_devices )
	MCFG_TI99_GROMPORT_ADD( GROMPORT_TAG, console_cartslot )

	/* Software list */
	MCFG_SOFTWARE_LIST_ADD("cart_list_ti99", "ti99_cart")

	/* Peripheral expansion box */
	MCFG_PERIBOX_ADD( PERIBOX_TAG, peribox_conf )

	/* sound hardware */
	MCFG_SPEAKER_STANDARD_MONO("mono")
	MCFG_SOUND_ADD(TISOUNDCHIP_TAG, SN94624, 3579545/8)	/* 3.579545 MHz */
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.75)
	MCFG_TI_SOUND_ADD( TISOUND_TAG )

	/* Cassette drives */
	MCFG_CASSETTE_ADD( CASSETTE_TAG, default_cassette_interface )
	MCFG_CASSETTE_ADD( CASSETTE2_TAG, default_cassette_interface )

	MCFG_SOUND_WAVE_ADD(WAVE_TAG, CASSETTE_TAG)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.25)

	/* GROM devices */
	MCFG_GROM_ADD( GROM0_TAG, grom0_config )
	MCFG_GROM_ADD( GROM1_TAG, grom1_config )
	MCFG_GROM_ADD( GROM2_TAG, grom2_config )

	/* User controller */
	MCFG_MECMOUSE_ADD( MECMOUSE_TAG, 60 )
MACHINE_CONFIG_END

static MACHINE_CONFIG_START( ti99_4a_50hz, ti99_4x )
	MCFG_CPU_ADD("maincpu", TMS9900, 3000000)
	MCFG_CPU_PROGRAM_MAP(memmap)
	MCFG_CPU_IO_MAP(cru_map)
	MCFG_MACHINE_START( ti99_4a )
	MCFG_MACHINE_RESET( ti99_4a )

	/* Video hardware */
	MCFG_TI_TMS991x_ADD_PAL(VIDEO_SYSTEM_TAG, TMS9929A, ti99_4_tms9928a_interface)

	/* Main board */
	MCFG_TMS9901_ADD(TMS9901_TAG, tms9901_wiring_ti99_4a, 3000000)
	MCFG_DMUX_ADD( DATAMUX_TAG, dmux_devices )
	MCFG_TI99_GROMPORT_ADD( GROMPORT_TAG, console_cartslot )

	/* Software list */
	MCFG_SOFTWARE_LIST_ADD("cart_list_ti99", "ti99_cart")

	/* Peripheral expansion box */
	MCFG_PERIBOX_ADD( PERIBOX_TAG, peribox_conf )

	/* sound hardware */
	MCFG_SPEAKER_STANDARD_MONO("mono")
	MCFG_SOUND_ADD(TISOUNDCHIP_TAG, SN94624, 3579545/8)	/* 3.579545 MHz */
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.75)
	MCFG_TI_SOUND_ADD( TISOUND_TAG )

	/* Cassette drives */
	MCFG_CASSETTE_ADD( CASSETTE_TAG, default_cassette_interface )
	MCFG_CASSETTE_ADD( CASSETTE2_TAG, default_cassette_interface )

	MCFG_SOUND_WAVE_ADD(WAVE_TAG, CASSETTE_TAG)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.25)

	/* GROM devices */
	MCFG_GROM_ADD( GROM0_TAG, grom0_config )
	MCFG_GROM_ADD( GROM1_TAG, grom1_config )
	MCFG_GROM_ADD( GROM2_TAG, grom2_config )

	/* User controller */
	MCFG_MECMOUSE_ADD( MECMOUSE_TAG, 50 )
MACHINE_CONFIG_END


TIMER_DEVICE_CALLBACK( ti99_4ev_hblank_interrupt )
{
	timer.machine().device<v9938_device>(V9938_TAG)->interrupt();
}

/*
    TI-99/4A with 80-column support. Actually a separate expansion card (EVPC),
    replacing the console video processor.
*/
static MACHINE_CONFIG_START( ti99_4ev_60hz, ti99_4x )
	MCFG_CPU_ADD("maincpu", TMS9900, 3000000)
	MCFG_CPU_PROGRAM_MAP(memmap_4ev)
	MCFG_CPU_IO_MAP(cru_map)
	MCFG_MACHINE_START( ti99_4a )

	/* Video hardware */
	// FIXME: (MZ) Lowered the screen rate to 30 Hz. This is a quick hack to
	// restore normal video speed for V9938-based systems until the V9938 implementation
	// is properly fixed.
	MCFG_TI_V9938_ADD(VIDEO_SYSTEM_TAG, 30, SCREEN_TAG, 2500, 512+32, (212+28)*2, DEVICE_SELF, ti99_4x, set_tms9901_INT2_from_v9938)
	MCFG_TIMER_ADD_SCANLINE("scantimer", ti99_4ev_hblank_interrupt, SCREEN_TAG, 0, 1)

	/* Main board */
	MCFG_TMS9901_ADD(TMS9901_TAG, tms9901_wiring_ti99_4a, 3000000)
	MCFG_DMUX_ADD( DATAMUX_TAG, dmux_devices )
	MCFG_TI99_GROMPORT_ADD( GROMPORT_TAG, console_cartslot )

	/* Software list */
	MCFG_SOFTWARE_LIST_ADD("cart_list_ti99", "ti99_cart")

	/* Peripheral expansion box */
	MCFG_PERIBOX_EV_ADD( PERIBOX_TAG, peribox_conf )

	/* sound hardware */
	MCFG_SPEAKER_STANDARD_MONO("mono")
	MCFG_SOUND_ADD(TISOUNDCHIP_TAG, SN94624, 3579545/8)	/* 3.579545 MHz */
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.75)
	MCFG_TI_SOUND_ADD( TISOUND_TAG )

	/* Cassette drives */
	MCFG_CASSETTE_ADD( CASSETTE_TAG, default_cassette_interface )
	MCFG_CASSETTE_ADD( CASSETTE2_TAG, default_cassette_interface )

	MCFG_SOUND_WAVE_ADD(WAVE_TAG, CASSETTE_TAG)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.25)

	/* GROM devices */
	MCFG_GROM_ADD( GROM0_TAG, grom0_config )
	MCFG_GROM_ADD( GROM1_TAG, grom1_config )
	MCFG_GROM_ADD( GROM2_TAG, grom2_config )
MACHINE_CONFIG_END


/*****************************************************************************
    ROM loading
    Note that we use the same ROMset for 50Hz and 60Hz versions.
    ROMs for peripheral equipment have been moved to the respective files.
******************************************************************************/
#define rom_ti99_4e rom_ti99_4
#define rom_ti99_4ae rom_ti99_4a

ROM_START(ti99_4)
	/*CPU memory space*/
	ROM_REGION16_BE(0x2000, "maincpu", 0)
	ROM_LOAD16_BYTE("u610.bin", 0x0000, 0x1000, CRC(6fcf4b15) SHA1(d085213c64701d429ae535f9a4ac8a50427a8343)) /* CPU ROMs high */
	ROM_LOAD16_BYTE("u611.bin", 0x0001, 0x1000, CRC(491c21d1) SHA1(7741ae9294c51a44a78033d1b77c01568a6bbfb9)) /* CPU ROMs low */

	/*GROM memory space*/
	ROM_REGION(0x10000, region_grom, 0)
	ROM_LOAD("u500.bin", 0x0000, 0x1800, CRC(aa757e13) SHA1(4658d3d01c0131c283a30cebd12e76754d41a84a)) /* system GROM 0 */
	ROM_LOAD("u501.bin", 0x2000, 0x1800, CRC(c863e460) SHA1(6d849a76011273a069a98ed0c3feaf13831c942f)) /* system GROM 1 */
	ROM_LOAD("u502.bin", 0x4000, 0x1800, CRC(b0eda548) SHA1(725e3f26f8c819f356e4bb405b4102b5ae1e0e70)) /* system GROM 2 */
ROM_END

ROM_START(ti99_4a)
	/*CPU memory space*/
	ROM_REGION16_BE(0x2000, "maincpu", 0)
	ROM_LOAD16_WORD("994arom.bin", 0x0000, 0x2000, CRC(db8f33e5) SHA1(6541705116598ab462ea9403c00656d6353ceb85)) /* system ROMs */

	/*GROM memory space*/
	ROM_REGION(0x10000, region_grom, 0)
	ROM_LOAD("994agrom.bin", 0x0000, 0x6000, CRC(af5c2449) SHA1(0c5eaad0093ed89e9562a2c0ee6a370bdc9df439)) /* system GROMs */
ROM_END

ROM_START(ti99_4ev)
	/*CPU memory space*/
	ROM_REGION16_BE(0x2000, "maincpu", 0)
	ROM_LOAD16_WORD("994arom.bin", 0x0000, 0x2000, CRC(db8f33e5) SHA1(6541705116598ab462ea9403c00656d6353ceb85)) /* system ROMs */

	/*GROM memory space*/
	ROM_REGION(0x10000, region_grom, 0)
	ROM_LOAD("994agr38.bin", 0x0000, 0x6000, CRC(bdd9f09b) SHA1(9b058a55d2528d2a6a69d7081aa296911ed7c0de)) /* system GROMs */
ROM_END

/*    YEAR  NAME      PARENT   COMPAT   MACHINE      INPUT    INIT      COMPANY             FULLNAME */
COMP( 1979, ti99_4,   0,	   0,		ti99_4_60hz,  ti99_4,   0,	"Texas Instruments", "TI99/4 Home Computer (US)" , 0)
COMP( 1980, ti99_4e,  ti99_4,  0,		ti99_4_50hz,  ti99_4,  0,	"Texas Instruments", "TI99/4 Home Computer (Europe)" , 0)
COMP( 1981, ti99_4a,  0,	   0,		ti99_4a_60hz, ti99_4a, 0,	"Texas Instruments", "TI99/4A Home Computer (US)" , 0)
COMP( 1981, ti99_4ae, ti99_4a, 0,		ti99_4a_50hz, ti99_4a, 0,	"Texas Instruments", "TI99/4A Home Computer (Europe)" , 0)
COMP( 1994, ti99_4ev, ti99_4a, 0,		ti99_4ev_60hz,ti99_4a, 0, "Texas Instruments", "TI99/4A Home Computer with EVPC" , 0)
