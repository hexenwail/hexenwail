/* Hexen II SERVERONLY table ABI: omitted loop and omitted client-only slots.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net_defs.h"
#include "net_dgrm.h"
#include "net_udp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern net_driver_t c_net_drivers[];
extern net_landriver_t c_net_landrivers[];
extern const int c_net_numdrivers,c_net_numlandrivers;
extern size_t H2NetDriver_sizeof(void), H2NetLandriver_sizeof(void);
extern size_t H2NetDriver_init_offset(void), H2NetDriver_shutdown_offset(void);
extern size_t H2NetLandriver_read_offset(void);
int net_hostport=26900;
qboolean tcpipAvailable;
char my_tcpip_address[NET_NAMELEN];
quakeparms_t parms;
quakeparms_t *host_parms=&parms;
int COM_CheckParm(const char *s) {(void)s;return 0;}
void CON_Printf(unsigned int flags,const char *fmt,...) {(void)flags;(void)fmt;}
void Sys_Error(const char *fmt,...) {fprintf(stderr,"fatal: %s\n",fmt);abort();}
int q_snprintf(char *dst,size_t size,const char *fmt,...) {
	int r;va_list ap;va_start(ap,fmt);r=vsnprintf(dst,size,fmt,ap);va_end(ap);return r;
}
int Datagram_Init(void) {return 0;}
void Datagram_Listen(qboolean x) {(void)x;}
qsocket_t *Datagram_CheckNewConnections(void) {return NULL;}
int Datagram_GetMessage(qsocket_t *s) {(void)s;return 0;}
int Datagram_SendMessage(qsocket_t *s,sizebuf_t *b) {(void)s;(void)b;return 0;}
int Datagram_SendUnreliableMessage(qsocket_t *s,sizebuf_t *b) {(void)s;(void)b;return 0;}
qboolean Datagram_CanSendMessage(qsocket_t *s) {(void)s;return true;}
qboolean Datagram_CanSendUnreliableMessage(qsocket_t *s) {(void)s;return true;}
void Datagram_Close(qsocket_t *s) {(void)s;}
void Datagram_Shutdown(void) {}
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL: dedicated table: %s\n",#x);return 1;} n++; } while(0)
int main(void) {
	int n=0;
	CHECK(c_net_numdrivers==1 && net_numdrivers==1);
	CHECK(c_net_numlandrivers==1 && net_numlandrivers==1);
	CHECK(H2NetDriver_sizeof()==sizeof(net_driver_t));
	CHECK(H2NetDriver_init_offset()==offsetof(net_driver_t,Init));
	CHECK(H2NetDriver_shutdown_offset()==offsetof(net_driver_t,Shutdown));
	CHECK(H2NetLandriver_sizeof()==sizeof(net_landriver_t));
	CHECK(H2NetLandriver_read_offset()==offsetof(net_landriver_t,Read));
	CHECK(strcmp(c_net_drivers[0].name,net_drivers[0].name)==0);
	CHECK(c_net_drivers[0].initialized==net_drivers[0].initialized);
#define D(field) CHECK(c_net_drivers[0].field==net_drivers[0].field)
	D(Init);D(Listen);D(CheckNewConnections);D(QGetMessage);
	D(QSendMessage);D(SendUnreliableMessage);D(CanSendMessage);
	D(CanSendUnreliableMessage);D(Close);D(Shutdown);
#undef D
	CHECK(strcmp(c_net_landrivers[0].name,net_landrivers[0].name)==0);
	CHECK(c_net_landrivers[0].initialized==net_landrivers[0].initialized);
	CHECK(c_net_landrivers[0].controlSock==net_landrivers[0].controlSock);
#define L(field) CHECK(c_net_landrivers[0].field==net_landrivers[0].field)
	L(Init);L(Shutdown);L(Listen);L(Open_Socket);L(Close_Socket);
	L(Connect);L(CheckNewConnections);L(Read);L(Write);L(Broadcast);
	L(AddrToString);L(StringToAddr);L(GetSocketAddr);L(GetNameFromAddr);
	L(GetAddrFromName);L(AddrCompare);L(GetSocketPort);L(SetSocketPort);
#undef L
	printf("dedicated driver tables: %d entries/layout/function-pointer checks passed\n",n);
	return 0;
}
