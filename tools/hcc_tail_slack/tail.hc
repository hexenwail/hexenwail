// The smallest HUD-only csprogs.dat the engine will load, laid out so the
// last global hcc allocates is a returned value.  OP_RETURN reads three
// globals from its operand, so without tail slack the image ends inside
// that read and PR_ValidateBytecode refuses it (GitHub #120).
// tools/test_hcc_tail_slack.py compiles and checks it.

void end_sys_globals;
void end_sys_fields;

void() CSQC_DrawHud =
{
};

// Declared last, so the immediate it returns is the last global.
float() tail_value =
{
	return 7;
};
