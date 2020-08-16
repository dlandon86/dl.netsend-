/* ------------------------ dl.netsend~ ----------------------------------------/
*                                                                              
* Sends uncompressed audio data over IP, from dl.netsend~ to dl.netreceive~.   
*                                                                              
* Copyright (C) 2020 David Landon                                                                              
*                                                                              
* dl.netsend~ utilizes Libuv and Pthread libraries, so those will be needed    
* if you intent to modify.                                                     
*                                                                              
* dl.netsend~ is free software: you can redistribute it and/or modify          
* it under the terms of the GNU General Public License as published by         
* the Free Software Foundation, either version 3 of the License, or            
* (at your option) any later version.                                          
*                                                                              
* dl.netsend~ is distributed in the hope that it will be useful,               
* but WITHOUT ANY WARRANTY; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY 
* OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  
* See the GNU General Public License for more details.                                 
*                                                                              
* <http://www.gnu.org/licenses/>
*
* ---------------------------------------------------------------------------- */


#include "uv.h" 

#include "ext.h"			// standard Max include, always required (except in Jitter)
#include "ext_obex.h"		// required for "new" style objects
#include "z_dsp.h"			// required for MSP objects

#define HAVE_STRUCT_TIMESPEC
#include "pthread.h"

#define DEFAULT_AUDIO_CHANNELS 1	    /* default number of audio channels */
#define MAXIMUM_AUDIO_CHANNELS 32	    /* max. number of audio channels we support */
#define DEFAULT_AUDIO_BUFFER_SIZE 1024	/* number of samples in one audio block */
#define DEFAULT_UDP_PACKT_SIZE 8192		/* number of bytes we send in one UDP datagram (OS X only) */
#define DEFAULT_IP_ADDRESS  "0.0.0.0"   /* default network port number */
#define DEFAULT_PORT "8000"             /* default network port number */
#define BUFMAX 4096

#define UV_ERROR(msg, code) do {                                           \
  post("%s: [%s: %s]\n", msg, uv_err_name((code)), uv_strerror((code)));   \
  assert(0);                                                               \
} while(0);


// struct to represent the object's state
typedef struct _dlnetsend {
	t_pxobject  		ob;			// the object itself (t_pxobject in MSP instead of t_object)
	double		    	d_offset; 	// the value of a property of our object
    long                d_channels;
    t_symbol           *d_ipaddr;
    t_symbol           *d_portno;
    t_double            d_vec;
    uv_udp_t            send_socket;
    uv_udp_send_t       send_req;
    uv_buf_t            buffer;
    struct sockaddr_in6  send_addr;
    uv_loop_t          *loop;

    pthread_t           thread;

    long                vs;

    //uv_loop_t           event_loop_struct;
    //uv_loop_t*          event_loop_ptr;


} t_dlnetsend;



uv_loop_t           event_loop_struct;
uv_loop_t*          event_loop_ptr;


uv_loop_t* uv_event_loop() {
    if (event_loop_ptr != NULL)
        return event_loop_ptr;

    if (uv_loop_init(&event_loop_struct))
        return NULL;

    event_loop_ptr = &event_loop_struct;
    return event_loop_ptr;
}


static t_symbol* ps_nothing, * ps_localhost;
static t_symbol* ps_format, * ps_channels, * ps_framesize, * ps_overflow, * ps_underflow;
static t_symbol* ps_queuesize, * ps_average, * ps_sf_float, * ps_sf_16bit, * ps_sf_8bit;
static t_symbol* ps_sf_mp3, * ps_sf_aac, * ps_sf_unknown, * ps_bitrate, * ps_hostname;



// method prototypes
void *dlnetsend_new(t_symbol *s, long argc, t_atom *argv);
void dlnetsend_free(t_dlnetsend *x);
void dlnetsend_assist(t_dlnetsend *x, void *b, long m, long a, char *s);
void dlnetsend_float(t_dlnetsend *x, double f);
void dlnetsend_dsp64(t_dlnetsend *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void dlnetsend_perform64(t_dlnetsend *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

void dlnetsend_int(t_dlnetsend* x, long n);
void send_cb(uv_udp_send_t* req, int status);
void test_msg(t_dlnetsend *x);

void sock_connect(t_dlnetsend* x);
void thread_main();



// global class pointer variable
static t_class *dlnetsend_class = NULL;


//***********************************************************************************************

void ext_main(void *r)
{
	// object initialization, note the use of dsp_free for the freemethod, which is required
	// unless you need to free allocated memory, in which case you should call dsp_free from
	// your custom free function.

	t_class *c = class_new("dl.netsend~", (method)dlnetsend_new, (method)dsp_free, (long)sizeof(t_dlnetsend), 0L, A_GIMME, 0);

	class_addmethod(c, (method)dlnetsend_float,		"float",	A_FLOAT, 0);
	class_addmethod(c, (method)dlnetsend_dsp64,		"dsp64",	A_CANT, 0);
	class_addmethod(c, (method)dlnetsend_assist,	    "assist",	A_CANT, 0);
    class_addmethod(c, (method)dlnetsend_int,         "int",      A_LONG, 0);

	class_dspinit(c);
	class_register(CLASS_BOX, c);
	dlnetsend_class = c;
}


void *dlnetsend_new(t_symbol *s, long argc, t_atom *argv)
{

	t_dlnetsend *x = (t_dlnetsend *)object_alloc(dlnetsend_class);


    t_atom* ap;
    ap = argv;

    ps_nothing = gensym("");
    ps_localhost = gensym("localhost");
    ps_hostname = gensym("ipaddr");
    ps_format = gensym("format");
    ps_channels = gensym("channels");
    ps_framesize = gensym("framesize");
    ps_bitrate = gensym("bitrate");
    ps_sf_float = gensym("_float_");
    ps_sf_16bit = gensym("_16bit_");
    ps_sf_8bit = gensym("_8bit_");
    ps_sf_mp3 = gensym("_mp3_");
    ps_sf_aac = gensym("_aac_");
    ps_sf_unknown = gensym("_unknown_");


	if (x) {

        x->d_offset = 0.0;

        x->d_channels = atom_getlong(ap);
        if (x->d_channels > 0 && x->d_channels <= MAXIMUM_AUDIO_CHANNELS)
        {
            post("netsend~: channels set to %d", x->d_channels);
        }
        else
        {
            x->d_channels = DEFAULT_AUDIO_CHANNELS;
            post("netsend~: Channel argument missing or outside allowable range. Channels set to %d", x->d_channels);
        }

		dsp_setup((t_pxobject *)x, x->d_channels);	// MSP inlets: arg is # of inlets and is REQUIRED!
		// use 0 if you don't need inlets
		outlet_new(x, "signal"); 		// signal outlet (note "signal" rather than NULL)

        // Set IP Address
        x->d_ipaddr = gensym(atom_getsym(ap + 1)->s_name);

        if (x->d_ipaddr->s_name != ps_nothing->s_name)
        {
            x->d_ipaddr = gensym(atom_getsym(ap + 1)->s_name);
            post("dl.netsend~: Ip address set to %s", x->d_ipaddr->s_name);
        }
        else
        {
            x->d_ipaddr = gensym(DEFAULT_IP_ADDRESS);
            post("dl.netsend~: IP Address argument missing. set to %s", x->d_ipaddr->s_name);
        }

        // Set port number
        x->d_portno = gensym(atom_getsym(ap + 2)->s_name);

        if (x->d_portno->s_name != ps_nothing->s_name)
        {
            x->d_portno = gensym(atom_getsym(ap + 2)->s_name);
            post("dl.netsend~: Port number set to %s", x->d_portno->s_name);
        }
        else {
            x->d_portno = gensym(DEFAULT_PORT);
            post("dl.netsend~: Port number argument missing. set to %s", x->d_portno->s_name);
        }
	}
	return (x);
}


// NOT CALLED!, we use dsp_free for a generic free function
void dlnetsend_free(t_dlnetsend *x)
{
	;
}


void dlnetsend_assist(t_dlnetsend *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { //inlet
		sprintf(s, "I am inlet %ld", a);
	}
	else {	// outlet
		sprintf(s, "I am outlet %ld", a);
	}
}


void dlnetsend_float(t_dlnetsend *x, double f)
{
	x->d_offset = f;
}


// registers a function for the signal chain in Max
void dlnetsend_dsp64(t_dlnetsend *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
	post("my sample rate is: %f", samplerate);

    x->vs = maxvectorsize;
    x->buffer.base = (char*)malloc(sizeof(double) * maxvectorsize);
    x->buffer.len = maxvectorsize;

object_method(dsp64, gensym("dsp_add64"), x, dlnetsend_perform64, 0, NULL);
}


// this is the 64-bit perform method audio vectors
void dlnetsend_perform64(t_dlnetsend *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
	t_double *inL = ins[0];		// we get audio for each inlet of the object from the **ins argument
	t_double *inR = ins[1];		// we get audio for each inlet of the object from the **ins argument
	t_double *outL = outs[0];	// we get audio for each outlet of the object from the **outs argument
	int n = sampleframes;

    memcpy(x->buffer.base, ins[0], sizeof(double) * x->vs);
    int r = uv_udp_send(&x->send_req, &x->send_socket, &x->buffer, 1, (const struct sockaddr*) & x->send_addr, send_cb);
    //int r = uv_udp_try_send(&x->send_socket, &x->buffer, 1, (const struct sockaddr*) & x->send_addr, send_cb);
    if (r) UV_ERROR("udp_send", r);


    // this perform method simply copies the input to the output, offsetting the value
	while (n--)
		*outL++ = *inL++ + x->d_offset;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void dlnetsend_int(t_dlnetsend* x, long n)
{
    sock_connect(x);
}

void send_cb(uv_udp_send_t* req, int status) {
    if (status) post("async send %s %s\n", uv_err_name(status), uv_strerror(status));
}

void test_msg(t_dlnetsend *x) {

    x->buffer = uv_buf_init("PING, the freak on!", 19);

    int r = uv_udp_send(&x->send_req, &x->send_socket, &x->buffer, 1, (const struct sockaddr*) &x->send_addr, send_cb);
    if (r) UV_ERROR("udp_send", r);

}

void sock_connect(t_dlnetsend* x) {

    //event_loop_ptr = (uv_loop_t*)malloc(sizeof(uv_loop_t));  // Shouldn't I be using this pointer? Doesn't work when I do...
    //event_loop_ptr = uv_event_loop();

    x->loop = (uv_loop_t*)malloc(sizeof(uv_loop_t));  // Move to the initilization routine...???
    x->loop = uv_event_loop();

    int r = uv_ip6_addr("0:0:0:0:0:0:0:1", 9123, &x->send_addr);
    if (r) UV_ERROR("ip6_addr", r)

    uv_udp_init(x->loop, &x->send_socket);
    uv_udp_bind(&x->send_socket, (const struct sockaddr*) &x->send_addr, 1);

    //test_msg(x);

    pthread_create(&x->thread, NULL, thread_main, NULL);
}

void thread_main() 
{
    post("dlnetsend: Opening loop");
    uv_run(uv_event_loop(), UV_RUN_DEFAULT);
    post("dlnetsend: loop closing");
}
