/* HexenWorld client networking integrated into Hexenwail. */
#ifndef HX2_CL_HW_H
#define HX2_CL_HW_H

#if defined(H2W_INTEGRATED)
qboolean HWCL_Connect (const char *host);
void HWCL_SendCmd (const usercmd_t *cmd);
void HWCL_ApplyState (void);
qboolean HWCL_Active (void);
void HWCL_Disconnect (void);
void HWCL_Frame (void);
void HWCL_Shutdown (void);
#endif

#endif /* HX2_CL_HW_H */
