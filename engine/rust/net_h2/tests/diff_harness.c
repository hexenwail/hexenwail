/* SPDX-License-Identifier: GPL-2.0-or-later
 * Differential transport fixtures: Hexen II only. The HexenWorld protocol
 * has its own gate; neither shares packet framing with this one.
 */
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net_defs.h"
#include "net_loop.h"
#include "net_udp.h"
#include "net_dgrm.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

extern net_driver_t c_net_drivers[];
extern net_landriver_t c_net_landrivers[];
extern const int c_net_numdrivers, c_net_numlandrivers;
extern size_t H2QSockAddr_sizeof(void), H2QSocket_sizeof(void);
extern size_t H2QSocket_receiveMessage_offset(void), H2QSocket_driverdata_offset(void);
extern size_t H2HostCache_sizeof(void), H2HostCache_addr_offset(void);
extern size_t H2NetDriver_sizeof(void), H2NetDriver_init_offset(void), H2NetDriver_shutdown_offset(void);
extern size_t H2NetLandriver_sizeof(void), H2NetLandriver_read_offset(void);
#define X(name) extern __typeof__(name) c_##name
X(Loop_Init); X(Loop_Shutdown); X(Loop_Listen); X(Loop_SearchForHosts);
X(Loop_Connect); X(Loop_CheckNewConnections); X(Loop_GetMessage);
X(Loop_SendMessage); X(Loop_SendUnreliableMessage); X(Loop_CanSendMessage);
X(Loop_CanSendUnreliableMessage); X(Loop_Close);
X(UDP_Init); X(UDP_Shutdown); X(UDP_Listen); X(UDP_OpenSocket); X(UDP_CloseSocket);
X(UDP_Connect); X(UDP_CheckNewConnections); X(UDP_Read); X(UDP_Write);
X(UDP_Broadcast); X(UDP_AddrToString); X(UDP_StringToAddr); X(UDP_GetSocketAddr);
X(UDP_GetNameFromAddr); X(UDP_GetAddrFromName); X(UDP_AddrCompare);
X(UDP_GetSocketPort); X(UDP_SetSocketPort);
#undef X

quakeparms_t parms;
quakeparms_t *host_parms = &parms;
client_static_t cls;
server_t sv;
server_static_t svs;
cvar_t hostname = {"hostname", "UNNAMED", CVAR_NONE};
int net_activeconnections = 3, net_driverlevel = 0;
int hostCacheCount;
hostcache_t hostcache[HOSTCACHESIZE];
int net_hostport = 26900;
char my_tcpip_address[NET_NAMELEN];
qboolean tcpipAvailable;
sizebuf_t net_message;
static unsigned char message_buf[NET_MAXMESSAGE];
static qsocket_t pool[4];
static int next_socket;

int Loop_TargetDedicated(void) { return cls.state == ca_dedicated; }
int Loop_TargetServerActive(void) { return sv.active; }
const char *Loop_TargetMapName(void) { return sv.name; }
int Loop_TargetMaxClients(void) { return svs.maxclients; }
const char *Loop_TargetHostname(void) { return hostname.string; }
qsocket_t *NET_NewQSocket(void) {
	if (next_socket == 4) return NULL;
	return &pool[next_socket++];
}
void SZ_Clear(sizebuf_t *buf) { buf->cursize = 0; }
void SZ_Write(sizebuf_t *buf, const void *data, int len) {
	if (len < 0 || len + buf->cursize > buf->maxsize) abort();
	memcpy(buf->data + buf->cursize, data, (size_t)len);
	buf->cursize += len;
}
int COM_CheckParm(const char *parm) {
	int i;
	for (i = 1; i < parms.argc; i++)
		if (strcmp(parms.argv[i], parm) == 0) return i;
	return 0;
}
static int c_arm;
static void record(const void *p,size_t n);
static void record_int(int x);
static void record_str(const char *s);
void CON_Printf(unsigned int flags, const char *fmt, ...) {
	char text[1024]; va_list ap;
	va_start(ap,fmt); vsnprintf(text,sizeof text,fmt,ap); va_end(ap);
	/* The C original is prefixed only to coexist in this harness binary.
	 * __thisfunc__ therefore prints c_UDP_* there, not in production. */
	if (c_arm && strncmp(text,"c_",2)==0)
		memmove(text,text+2,strlen(text+2)+1);
	record_int((int)flags); record_str(text);
}
void Sys_Error(const char *fmt, ...) { fprintf(stderr,"fatal: %s\n",fmt); abort(); }
int q_snprintf(char *dst, size_t size, const char *fmt, ...) {
	va_list ap; int ret; va_start(ap,fmt); ret = vsnprintf(dst,size,fmt,ap); va_end(ap); return ret;
}
#define DG(ret,name,args,body) ret name args body
DG(int, Datagram_Init, (void), {return 0;})
DG(void, Datagram_Listen, (qboolean state), {(void)state;})
DG(void, Datagram_SearchForHosts, (qboolean xmit), {(void)xmit;})
DG(qsocket_t *, Datagram_Connect, (const char *host), {(void)host;return NULL;})
DG(qsocket_t *, Datagram_CheckNewConnections, (void), {return NULL;})
DG(int, Datagram_GetMessage, (qsocket_t *sock), {(void)sock;return 0;})
DG(int, Datagram_SendMessage, (qsocket_t *sock,sizebuf_t *buf), {(void)sock;(void)buf;return 0;})
DG(int, Datagram_SendUnreliableMessage, (qsocket_t *sock,sizebuf_t *buf), {(void)sock;(void)buf;return 0;})
DG(qboolean, Datagram_CanSendMessage, (qsocket_t *sock), {(void)sock;return true;})
DG(qboolean, Datagram_CanSendUnreliableMessage, (qsocket_t *sock), {(void)sock;return true;})
DG(void, Datagram_Close, (qsocket_t *sock), {(void)sock;})
DG(void, Datagram_Shutdown, (void), {})
#undef DG

static unsigned char trace[131072];
static size_t used;
static int checks;
static int failed;
static void record(const void *p,size_t n) {
	if (used + n > sizeof(trace)) abort();
	memcpy(trace+used,p,n); used+=n;
}
static void record_int(int x) { record(&x,sizeof x); checks++; }
static void record_str(const char *s) { record(s,strlen(s)+1); checks++; }
static void verify(int b) { record_int(b); if (!b) failed++; }
#define DRIVER(f) verify(c ? c_net_drivers[0].f == c_Loop_##f : net_drivers[0].f == Loop_##f)
#define LAND(f) verify(c ? c_net_landrivers[0].f == c_UDP_##f : net_landrivers[0].f == UDP_##f)
static void tables(int c) {
	verify(H2QSockAddr_sizeof()==sizeof(struct qsockaddr));
	verify(H2QSocket_sizeof()==sizeof(qsocket_t));
	verify(H2QSocket_driverdata_offset()==offsetof(qsocket_t,driverdata));
	verify(H2QSocket_receiveMessage_offset()==offsetof(qsocket_t,receiveMessage));
	verify(H2HostCache_sizeof()==sizeof(hostcache_t));
	verify(H2HostCache_addr_offset()==offsetof(hostcache_t,addr));
	verify(H2NetDriver_sizeof()==sizeof(net_driver_t));
	verify(H2NetDriver_init_offset()==offsetof(net_driver_t,Init));
	verify(H2NetDriver_shutdown_offset()==offsetof(net_driver_t,Shutdown));
	verify(H2NetLandriver_sizeof()==sizeof(net_landriver_t));
	verify(H2NetLandriver_read_offset()==offsetof(net_landriver_t,Read));
	net_driver_t *driver = c ? c_net_drivers : net_drivers;
	net_landriver_t *land = c ? c_net_landrivers : net_landrivers;
	record_int(c ? c_net_numdrivers : net_numdrivers);
	record_int(c ? c_net_numlandrivers : net_numlandrivers);
	for (int i=0;i<2;i++) {
		record_str(driver[i].name); record_int(driver[i].initialized);
		verify(driver[i].Init == (i ? Datagram_Init : (c ? c_Loop_Init : Loop_Init)));
		verify(driver[i].Listen == (i ? Datagram_Listen : (c ? c_Loop_Listen : Loop_Listen)));
		verify(driver[i].SearchForHosts == (i ? Datagram_SearchForHosts : (c ? c_Loop_SearchForHosts : Loop_SearchForHosts)));
		verify(driver[i].Connect == (i ? Datagram_Connect : (c ? c_Loop_Connect : Loop_Connect)));
		verify(driver[i].CheckNewConnections == (i ? Datagram_CheckNewConnections : (c ? c_Loop_CheckNewConnections : Loop_CheckNewConnections)));
		verify(driver[i].QGetMessage == (i ? Datagram_GetMessage : (c ? c_Loop_GetMessage : Loop_GetMessage)));
		verify(driver[i].QSendMessage == (i ? Datagram_SendMessage : (c ? c_Loop_SendMessage : Loop_SendMessage)));
		verify(driver[i].SendUnreliableMessage == (i ? Datagram_SendUnreliableMessage : (c ? c_Loop_SendUnreliableMessage : Loop_SendUnreliableMessage)));
		verify(driver[i].CanSendMessage == (i ? Datagram_CanSendMessage : (c ? c_Loop_CanSendMessage : Loop_CanSendMessage)));
		verify(driver[i].CanSendUnreliableMessage == (i ? Datagram_CanSendUnreliableMessage : (c ? c_Loop_CanSendUnreliableMessage : Loop_CanSendUnreliableMessage)));
		verify(driver[i].Close == (i ? Datagram_Close : (c ? c_Loop_Close : Loop_Close)));
		verify(driver[i].Shutdown == (i ? Datagram_Shutdown : (c ? c_Loop_Shutdown : Loop_Shutdown)));
	}
	record_str(land[0].name); record_int(land[0].initialized); record_int(land[0].controlSock);
	LAND(Init); LAND(Shutdown); LAND(Listen);
#define LAND_AS(field,func) verify(c ? c_net_landrivers[0].field == c_UDP_##func : net_landrivers[0].field == UDP_##func)
	LAND_AS(Open_Socket,OpenSocket); LAND_AS(Close_Socket,CloseSocket);
	LAND(Connect); LAND(CheckNewConnections); LAND(Read); LAND(Write);
	LAND(Broadcast); LAND(AddrToString); LAND(StringToAddr); LAND(GetSocketAddr);
	LAND(GetNameFromAddr); LAND(GetAddrFromName); LAND(AddrCompare);
	LAND(GetSocketPort); LAND(SetSocketPort);
#undef LAND_AS
}
#define CALL(name) (c ? c_Loop_##name : Loop_##name)
static void loop(int c) {
	char payload[]="loop-hello";
	sizebuf_t data={0}; qsocket_t *client,*server;
	data.data=(byte *)payload; data.cursize=(int)strlen(payload);
	data.maxsize=sizeof(payload);
	strcpy(sv.name,"demo1"); sv.active=1; svs.maxclients=8;
	net_message.data=message_buf; net_message.maxsize=sizeof(message_buf);
	record_int(CALL(Init)());
	CALL(SearchForHosts)(false);
	record_int(hostCacheCount); record_str(hostcache[0].name);
	record_str(hostcache[0].map); record_str(hostcache[0].cname);
	record_int(hostcache[0].users); record_int(hostcache[0].maxusers);
	verify(CALL(Connect)("elsewhere") == NULL);
	client=CALL(Connect)("local"); verify(client != NULL);
	server=CALL(CheckNewConnections)(); verify(server != NULL);
	verify(CALL(CheckNewConnections)() == NULL);
	record_int(CALL(SendMessage)(client,&data));
	record_int(client->canSend); record_int(server->receiveMessageLength);
	record(server->receiveMessage,(size_t)server->receiveMessageLength);
	record_int(CALL(GetMessage)(server));
	record_int(net_message.cursize); record(net_message.data,(size_t)net_message.cursize);
	record_int(client->canSend);
	record_int(CALL(SendUnreliableMessage)(server,&data));
	record_int(CALL(GetMessage)(client));
	record_int(net_message.cursize); record(net_message.data,(size_t)net_message.cursize);
	CALL(Close)(client); record_int(CALL(CanSendMessage)(server));
	CALL(Close)(server); CALL(Shutdown)();
}
#undef CALL
#define UDP(name) (c ? c_UDP_##name : UDP_##name)
static void udp_init(int c) {
	char *args[]={"harness","-noifscan","-ip","127.0.0.1","-localip","127.0.0.1",NULL};
	struct qsockaddr addr={0};
	int fd;
	parms.argc=6; parms.argv=args;
	net_hostport=0;
	fd=UDP(Init)(); verify(fd!=INVALID_SOCKET);
	record_int(tcpipAvailable); record_str(my_tcpip_address);
	if(fd!=INVALID_SOCKET) {
		UDP(GetSocketAddr)(fd,&addr);
		verify(UDP(GetSocketPort)(&addr)>0);
		verify(((struct sockaddr_in *)&addr)->sin_addr.s_addr==htonl(INADDR_LOOPBACK));
		UDP(Listen)(true);
		UDP(Listen)(false);
		UDP(Shutdown)();
	}
}
static void udp(int c) {
	struct qsockaddr a={0},b={0};
	int port;
	record_int(UDP(StringToAddr)("127.0.0.1:26000", &a));
	record_str(UDP(AddrToString)(&a));
	record_int(UDP(GetSocketPort)(&a));
	record_int(UDP(GetAddrFromName)("bad-name.invalid",&b));
	record_int(UDP(GetAddrFromName)("999",&b));
	record_int(UDP(GetAddrFromName)("127.0.0.1:26001",&b));
	record_str(UDP(AddrToString)(&b));
	record_int(UDP(AddrCompare)(&a,&b));
	UDP(SetSocketPort)(&b,26000); record_int(UDP(AddrCompare)(&a,&b));
	/* Invalid descriptor paths must preserve both errno diagnostics and return
	 * values. Broadcast fails before sending to the network. */
	byte bad[]="x";
	record_int(UDP(Read)(-1,bad,1,&a));
	record_int(UDP(Write)(-1,bad,1,&a));
	record_int(UDP(Broadcast)(-1,bad,1));
	port=UDP(OpenSocket)(0);
	verify(port != INVALID_SOCKET);
	if (port != INVALID_SOCKET) {
		struct sockaddr_in actual; socklen_t len=sizeof actual;
		getsockname(port,(struct sockaddr *)&actual,&len);
		int other=UDP(OpenSocket)(ntohs(actual.sin_port));
		verify(other == INVALID_SOCKET);
		if (other != INVALID_SOCKET) UDP(CloseSocket)(other);
		record_int(UDP(Connect)(port,&a));
		record_int(UDP(Read)(port,(byte *)&actual,1,&a));

		/* A plain C peer on the wire, not a second call to UDP_Write.
		 * Verify both directions' bytes and the source address of the reply. */
		int peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		struct sockaddr_in peer_addr={0},target_addr={0};
		socklen_t peer_len=sizeof peer_addr;
		peer_addr.sin_family=AF_INET; peer_addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
		verify(peer >= 0 && bind(peer,(struct sockaddr *)&peer_addr,sizeof peer_addr)==0);
		getsockname(peer,(struct sockaddr *)&peer_addr,&peer_len);
		target_addr.sin_family=AF_INET; target_addr.sin_port=actual.sin_port;
		target_addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
		char outgoing[]="Rust or C -> peer", incoming[]="peer -> Rust or C";
		struct qsockaddr peer_qaddr;
		memcpy(&peer_qaddr,&peer_addr,sizeof peer_qaddr);
		record_int(UDP(Write)(port,(byte *)outgoing,(int)strlen(outgoing),&peer_qaddr));
		char buf[128]={0};
		struct timeval tv={.tv_sec=1};
		setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
		int got=(int)recvfrom(peer,buf,sizeof buf,0,NULL,NULL);
		record_int(got); if(got>0) record(buf,(size_t)got);
		sendto(peer,incoming,strlen(incoming),0,(struct sockaddr *)&target_addr,sizeof target_addr);
		struct qsockaddr from={0}; got=0;
		for (int retries=0;retries<100 && got==0;retries++) {
			got=UDP(Read)(port,(byte *)buf,sizeof buf,&from);
			if (got==0) usleep(1000);
		}
		record_int(got); if(got>0) record(buf,(size_t)got);
		verify(UDP(GetSocketPort)(&from)==ntohs(peer_addr.sin_port));
		verify(((struct sockaddr_in *)&from)->sin_addr.s_addr == peer_addr.sin_addr.s_addr);
		close(peer);
		UDP(CloseSocket)(port);
	}
}
#undef UDP

struct result { size_t used; int checks, failed; unsigned char trace[131072]; };
static void run_child(int fd,int c, int scenario) {
	struct result result;
	parms.argc=1; c_arm=c;
	if (scenario==0) tables(c);
	if (scenario==1) loop(c);
	if (scenario==2) udp(c);
	if (scenario==3) udp_init(c);
	result.used=used; result.checks=checks; result.failed=failed;
	memcpy(result.trace, trace, used);
	if (write(fd,&result,offsetof(struct result,trace)+used) < 0) _exit(3);
	_exit(0);
}
int main(void) {
	int total=0; size_t total_bytes=0;
	const char *names[]={"driver tables", "loopback framing", "UDP address/failure paths", "UDP init/shutdown"};
	for(int scenario=0;scenario<4;scenario++) {
		struct result results[2]; memset(results,0,sizeof(results));
		for (int c=0;c<2;c++) {
			int fds[2],status; pid_t pid;
			if(pipe(fds)!=0) abort();
			pid=fork(); if (pid==0) {close(fds[0]);run_child(fds[1],c,scenario);}
			close(fds[1]); ssize_t n=read(fds[0],&results[c],sizeof(results[c]));
			close(fds[0]);waitpid(pid,&status,0);
			if(n<(ssize_t)offsetof(struct result,trace) || !WIFEXITED(status) || WEXITSTATUS(status)) {
				fprintf(stderr,"%s %s child failed: %zd status %d\n",names[scenario],c?"Rust":"C",n,status);return 1;
			}
		}
		if (results[0].failed || results[1].failed || results[0].checks!=results[1].checks ||
			results[0].used!=results[1].used || memcmp(results[0].trace,results[1].trace,results[0].used)) {
			fprintf(stderr,"FAIL: %s Rust=%zu/%d/%d C=%zu/%d/%d\n",names[scenario],
				results[0].used,results[0].checks,results[0].failed,
				results[1].used,results[1].checks,results[1].failed);
			for (size_t i=0;i<results[0].used && i<results[1].used;i++) {
				if(results[0].trace[i]!=results[1].trace[i]) {
					fprintf(stderr,"  first difference at %zu: Rust='%.*s' C='%.*s'\n",i,
						40,results[0].trace+i,40,results[1].trace+i);break;
				}
			}
			return 1;
		}
		total+=results[0].checks; total_bytes+=results[0].used;
		printf("PASS: %s (%d expectations, %zu trace bytes)\n",names[scenario],results[0].checks,results[0].used);
	}
	printf("checked %d expectations, 4 cases, %zu trace bytes\nRESULT: PASS\n",total,total_bytes);
	return 0;
}
