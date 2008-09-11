/*****************************************************************************
 *
 * includes/ondra.h
 *
 ****************************************************************************/

#ifndef ONDRA_H_
#define ONDRA_H_

/*----------- defined in machine/ondra.c -----------*/

extern DRIVER_INIT( ondra );
extern MACHINE_START( ondra );
extern MACHINE_RESET( ondra );

extern WRITE8_HANDLER( ondra_port_03_w );
extern WRITE8_HANDLER( ondra_port_09_w );
extern WRITE8_HANDLER( ondra_port_0a_w );
/*----------- defined in video/ondra.c -----------*/

extern VIDEO_START( ondra );
extern VIDEO_UPDATE( ondra );

#endif
