//      ao.c
//      
//      Copyright 2010 Ilya <ilya@laptop>
//      
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; either version 2 of the License, or
//      (at your option) any later version.
//      
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//      
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
//      MA 02110-1301, USA.


#include "ao.h"

struct AOutput * aout_init(void) {
	struct AOutput * ao = (struct AOutput *) malloc(sizeof(struct AOutput));
	
	ao_initialize();
	ao->default_driver = ao_default_driver_id();
	ao->format.bits = 16;
	ao->format.channels = 2;
	ao->format.rate = 44100;
	ao->format.byte_format = AO_FMT_LITTLE;
	ao->format.matrix = "L,R";
	ao->device = ao_open_live(ao->default_driver, &(ao->format), NULL);

	return ao;
}


void aout_play(struct AOutput * ao, void * buffer, int buffer_size) {
	ao_play(ao->device, buffer, buffer_size);
}


void aout_close(struct AOutput * ao) {
	ao_close(ao->device);
	ao_shutdown();
}
