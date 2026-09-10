/* HexenWorld client networking integrated into Hexenwail. */
#ifndef HX2_CL_HW_H
#define HX2_CL_HW_H

#if defined(H2W_INTEGRATED)
/* Longest server address the menu accepts: Host_Connect_f copies its argument
 * into a MAX_QPATH (64) buffer, and the join prepends "hw://". */
#define HW_ADDRESS_MAX	(MAX_QPATH - 1 - 5)

/* menu.c: add an address the client reached to the HexenWorld server list. */
void M_HW_RememberServer (const char *address);

/* Userinfo "spectator": empty or "0" joins as a player, anything else as a
 * spectator (the value doubles as the spectator password).  hwsv reads it
 * only at connect. */
extern cvar_t hw_spectator;

void HWCL_Init (void);
void HWCL_SetInfo (const char *key, const char *value);
qboolean HWCL_Connect (const char *host);
void HWCL_SendCmd (const usercmd_t *cmd, int buttons, int impulse);
void HWCL_ApplyState (void);
void HWCL_PredictUsercmd (const usercmd_t *cmd, int buttons, int impulse);
qboolean HWCL_Active (void);
void HWCL_Disconnect (void);
void HWCL_Frame (void);
void HWCL_Shutdown (void);
#endif

#endif /* HX2_CL_HW_H */
