#!/usr/bin/env python3
"""Data-free regression tests for the production join and HW search-path code.

Extract the actual C functions, stub their engine dependencies, and compile/run
with the host C compiler. No renderer, proprietary paks, or running server needed.
Runtime gameplay coverage remains in the multiplayer smoke tests.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
#include <stdarg.h>
#define H2W_INTEGRATED 1
#define MAX_QPATH 64
#define MAX_OSPATH 256
#define __thisfunc__ __func__
typedef int qboolean;
#define true 1
#define false 0
#define q_strcasecmp strcasecmp
#define q_strncasecmp strncasecmp
static void q_strlcpy(char *d, const char *s, size_t n) { snprintf(d,n,"%s",s); }
'''

FS_STUBS = r'''
typedef struct path_s { struct path_s *next; char name[64]; } searchpath_t;
static searchpath_t base = {NULL,"data1"}, local = {&base,"localmod"};
static searchpath_t *fs_searchpaths = &local, *fs_base_searchpaths = &base;
static char fs_gamedir_nopath[64] = "localmod";
static char fs_gamedir[256] = "/base/localmod", fs_userdir[256] = "/user/localmod";
static unsigned int gameflags = 123;
static int added, freed;
static void Cache_Flush(void) {}
static void FS_UnwindSearchpaths(searchpath_t *mark, qboolean verbose) {
    (void)verbose;
    while (fs_searchpaths != mark) {
        assert(fs_searchpaths != &local && fs_searchpaths != &base);
        searchpath_t *next = fs_searchpaths->next;
        free(fs_searchpaths); freed++; fs_searchpaths = next;
    }
}
static void FS_AddGameDirectory(const char *dir, qboolean base_fs) {
    (void)base_fs;
    searchpath_t *p = calloc(1,sizeof(*p)); assert(p);
    q_strlcpy(p->name,dir,sizeof(p->name)); p->next=fs_searchpaths; fs_searchpaths=p;
    q_strlcpy(fs_gamedir_nopath,dir,sizeof(fs_gamedir_nopath));
    snprintf(fs_gamedir,sizeof(fs_gamedir),"/base/%s",dir);
    snprintf(fs_userdir,sizeof(fs_userdir),"/user/%s",dir);
    gameflags |= 256; added++;
}
'''

FS_TESTS = r'''
int main(void) {
    const char *bad[] = {"", ".", "..", "../siege", "a/b", "a\\b", "C:siege",
        "data1", "PORTALS", "siege\nquit", "siege;quit", "a\"b", "a b", "a..b"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        assert(!FS_HWGamedir(bad[i])); assert(fs_searchpaths == &local);
    }
    char longdir[80]; memset(longdir,'a',79); longdir[79]=0;
    assert(!FS_HWGamedir(longdir));
    assert(FS_HWGamedir("siege"));
    assert(!strcmp(fs_searchpaths->name,"siege"));
    assert(!strcmp(fs_searchpaths->next->name,"hw"));
    assert(fs_searchpaths->next->next == &base); /* local mod excluded */
    assert(!strcmp(fs_userdir,"/user/siege"));
    searchpath_t *unchanged = fs_searchpaths;
    assert(!FS_HWGamedir("../bad")); assert(fs_searchpaths == unchanged);
    assert(FS_HWGamedir("hw")); /* map/server switch removes previous mod */
    assert(!strcmp(fs_searchpaths->name,"hw")); assert(fs_searchpaths->next == &base);
    assert(FS_HWGamedir("siege")); /* reconnect does not accumulate paths */
    assert(FS_HWGamedir("siege"));
    FS_HWRestore();
    assert(fs_searchpaths == &local && local.next == &base);
    assert(!strcmp(fs_gamedir_nopath,"localmod"));
    assert(!strcmp(fs_gamedir,"/base/localmod"));
    assert(!strcmp(fs_userdir,"/user/localmod")); assert(gameflags == 123);
    FS_HWRestore(); assert(added == freed); /* idempotent disconnect */
    puts("PASS: safe HW/mod layering, invalid gamedirs, repeated switches and exact restoration");
}
'''

JOIN_STUBS = r'''
enum { ca_disconnected, ca_connected, ca_dedicated, clc_nop };
static struct { int state,demoplayback,demonum,signon,message; void *netcon; } cls;
static struct { double last_received_message; } cl;
static struct { int integer; } cl_shownet;
static qboolean net_connect_no_response, m_return_onerror;
static int net_calls, hw_calls, net_result, hw_result=1, disconnects;
static char last_net[128], last_hw[128];
static double realtime;
static int reads[4], readpos, parse_signon;
#define CL_AUTO_H2_CONFIRM_TIMEOUT 5.0
static qboolean cl_auto_h2_pending;
static double cl_auto_h2_started;
static char cl_auto_h2_host[MAX_QPATH];
static jmp_buf abort_join;
static void CL_Disconnect(void) { disconnects++; cl_auto_h2_pending=false; }
static void *NET_Connect(const char *h) {
    net_calls++; q_strlcpy(last_net,h,sizeof(last_net));
    return net_result ? &cls : NULL;
}
static qboolean HWCL_Connect(const char *h) {
    hw_calls++; q_strlcpy(last_hw,h,sizeof(last_hw)); return hw_result;
}
static void HWCL_Frame(void) {}
static qboolean HWCL_Active(void) { return false; }
static void HWCL_ApplyState(void) {}
static void CL_AdvanceTime(void) {}
static int CL_GetMessage(void) { assert(readpos < 4); return reads[readpos++]; }
static void CL_ParseServerMessage(void) { if (parse_signon) cls.signon=1; }
static void CL_RelinkEntities(void) {}
static void CL_UpdateEffects(void) {}
static void CL_UpdateTEnts(void) {}
static void CL_UpdateDevStats(void) {}
static void Host_Error(const char *fmt,...) { (void)fmt; longjmp(abort_join,1); }
static void Con_Printf(const char *fmt,...) { (void)fmt; }
#define Con_DPrintf Con_Printf
static void MSG_WriteByte(int *m,int b) { *m=b; }
static void reset(void) {
    memset(&cls,0,sizeof(cls)); memset(&cl,0,sizeof(cl));
    memset(reads,0,sizeof(reads)); readpos=parse_signon=0;
    net_calls=hw_calls=disconnects=0; realtime=100; cl_shownet.integer=0;
    net_result=0; hw_result=1; net_connect_no_response=0; m_return_onerror=1;
    cl_auto_h2_pending=false; cl_auto_h2_started=0; cl_auto_h2_host[0]=0;
}
'''

JOIN_TESTS = r'''
int main(void) {
    reset(); net_result=1; CL_EstablishConnection("host:26900");
    assert(net_calls==1 && hw_calls==0 && cls.state==ca_connected);
    assert(cl_auto_h2_pending && cl_auto_h2_started==realtime);
    assert(!strcmp(cl_auto_h2_host,"host:26900"));
    realtime += 4.9; assert(!CL_AutoProtocolFallback(false));
    realtime += .2; assert(CL_AutoProtocolFallback(false));
    assert(disconnects==2 && hw_calls==1 && !strcmp(last_hw,"host:26900"));
    reset(); cl_auto_h2_pending=true; cl_auto_h2_started=realtime;
    q_strlcpy(cl_auto_h2_host,"dropped",sizeof(cl_auto_h2_host));
    assert(CL_AutoProtocolFallback(true)); /* provisional qsocket died early */
    assert(hw_calls==1 && !strcmp(last_hw,"dropped"));
    reset(); net_connect_no_response=1; CL_EstablishConnection("host:26950");
    assert(net_calls==1 && hw_calls==1 && !strcmp(last_hw,"host:26950"));
    assert(!m_return_onerror);
    reset(); CL_EstablishConnection("hw://host:26950");
    assert(net_calls==0 && hw_calls==1 && !strcmp(last_hw,"host:26950"));
    reset(); net_result=1; CL_EstablishConnection("h2://host:26900");
    assert(net_calls==1 && hw_calls==0 && !strcmp(last_net,"host:26900"));
    assert(!cl_auto_h2_pending); realtime += 10; assert(!CL_AutoProtocolFallback(false));
    reset(); /* an H2 rejection/error must not try another protocol */
    if (!setjmp(abort_join)) { CL_EstablishConnection("host"); assert(0); }
    assert(net_calls==1 && hw_calls==0 && m_return_onerror);
    reset(); net_connect_no_response=1;
    if (!setjmp(abort_join)) { CL_EstablishConnection("h2://host"); assert(0); }
    assert(hw_calls==0);
    reset(); net_connect_no_response=1;
    if (!setjmp(abort_join)) { CL_EstablishConnection("local"); assert(0); }
    assert(hw_calls==0);
    reset(); hw_result=0;
    if (!setjmp(abort_join)) { CL_EstablishConnection("hw://bad"); assert(0); }
    assert(net_calls==0);

    reset(); cls.netcon=&cls; cls.state=ca_connected; cl_auto_h2_pending=true;
    cl_auto_h2_started=90; q_strlcpy(cl_auto_h2_host,"queued",sizeof(cl_auto_h2_host));
    reads[0]=1; reads[1]=0; parse_signon=1; CL_ReadFromServer();
    assert(cls.signon==1 && !cl_auto_h2_pending && hw_calls==0 && readpos==2);
    reset(); cls.netcon=&cls; cls.state=ca_connected; cl_auto_h2_pending=true;
    cl_auto_h2_started=90; q_strlcpy(cl_auto_h2_host,"nop",sizeof(cl_auto_h2_host));
    reads[0]=1; reads[1]=0; CL_ReadFromServer();
    assert(hw_calls==1 && !strcmp(last_hw,"nop")); /* svc_nop cannot confirm H2 */
    reset(); cls.netcon=&cls; cls.state=ca_connected; cl_auto_h2_pending=true;
    cl_auto_h2_started=realtime; q_strlcpy(cl_auto_h2_host,"dropped",sizeof(cl_auto_h2_host));
    reads[0]=-1; CL_ReadFromServer(); assert(hw_calls==1); /* early socket loss */
    puts("PASS: automatic selection, overrides, rejection handling, and provisional H2 lifecycle");
}
'''


def main():
    fs = (ROOT / "engine/h2shared/quakefs.c").read_text()
    start = fs.index("static searchpath_t *fs_hw_saved_paths;")
    end = fs.index("\n#endif", start)
    cases = {
        "paths": PRELUDE + FS_STUBS + fs[start:end] + FS_TESTS,
        "join": PRELUDE + JOIN_STUBS + function(
            "engine/hexen2/cl_main.c", "static qboolean CL_AutoProtocolFallback (qboolean force)"
        ) + function(
            "engine/hexen2/cl_main.c", "void CL_EstablishConnection (const char *host)"
        ) + function(
            "engine/hexen2/cl_main.c", "int CL_ReadFromServer (void)"
        ) + JOIN_TESTS,
    }
    with tempfile.TemporaryDirectory(prefix="multiplayer-join-") as tmp:
        for name, source in cases.items():
            cfile = Path(tmp) / (name + ".c")
            binary = Path(tmp) / name
            cfile.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                            str(cfile), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
