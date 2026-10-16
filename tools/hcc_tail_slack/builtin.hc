// A csprogs.dat whose last declaration is a builtin.  A builtin allocates no
// globals, but its function record puts parm_start where the globals stood
// when it was declared, and the engine checks its parameter words from there:
// three for a vector.  Without that much tail the image is refused (#120).
// hcc's builtin syntax; builtin_qcc.hc is the same image for dhcc.

void end_sys_globals;
void end_sys_fields;

void() CSQC_DrawHud =
{
};

void tail_builtin(vector v) : 99;
